#include "ss_json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static const char *skip_string(const char *s) {
    if (*s != '"') return s;
    s++;
    while (*s) {
        if (*s == '\\') { if (s[1]) s += 2; else s++; continue; }
        if (*s == '"') return s + 1;
        s++;
    }
    return s;
}

// Skips one JSON value of any kind and returns the character after it.
static const char *skip_value(const char *s) {
    s = skip_ws(s);
    if (*s == '"') return skip_string(s);
    if (*s == '{' || *s == '[') {
        int depth = 0;
        while (*s) {
            if (*s == '"') { s = skip_string(s); continue; }
            if (*s == '{' || *s == '[') depth++;
            else if (*s == '}' || *s == ']') {
                depth--;
                if (depth == 0) return s + 1;
            }
            s++;
        }
        return s;
    }
    while (*s && *s != ',' && *s != '}' && *s != ']' && !isspace((unsigned char)*s)) s++;
    return s;
}

// Finds `key` among the members of the object `json` starts with (its own
// keys only: a key with the same name inside a nested object or array, or
// inside a string, is never matched). Returns the start of the value, or NULL.
static const char *find_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    const char *p = skip_ws(json);
    if (*p != '{') {
        p = strchr(p, '{');
        if (!p) return NULL;
    }
    p++;
    size_t klen = strlen(key);
    while (*p) {
        p = skip_ws(p);
        if (*p == '}' || !*p) return NULL;
        if (*p == ',') { p++; continue; }
        if (*p != '"') return NULL;
        const char *kstart = p + 1;
        const char *kend = skip_string(p);
        int match = (size_t)(kend - 1 - kstart) == klen && strncmp(kstart, key, klen) == 0;
        p = skip_ws(kend);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);
        if (match) return p;
        p = skip_value(p);
    }
    return NULL;
}

static int is_null(const char *v) {
    return v && strncmp(v, "null", 4) == 0;
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int put_utf8(char *out, int room, unsigned cp) {
    char buf[4];
    int n;
    if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (n > room) return -1;
    memcpy(out, buf, n);
    return n;
}

int json_get_string(const char *json, const char *key, char *out, int out_size) {
    if (!out || out_size <= 0) return -1;
    const char *v = find_key(json, key);
    if (!v || *v != '"') return -1;
    v++;
    int len = 0;
    // Escapes are decoded (\" \\ \/ \n \t \r \b \f \uXXXX, surrogate pairs
    // included). A value longer than out is cut at a whole character.
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) {
            char e = v[1];
            v += 2;
            char simple;
            switch (e) {
                case 'n': simple = '\n'; break;
                case 't': simple = '\t'; break;
                case 'r': simple = '\r'; break;
                case 'b': simple = '\b'; break;
                case 'f': simple = '\f'; break;
                case 'u': {
                    unsigned cp = 0, lo = 0;
                    if (hex4(v, &cp) != 0) { simple = '?'; break; }
                    v += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && v[0] == '\\' && v[1] == 'u' && hex4(v + 2, &lo) == 0
                        && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        v += 6;
                    }
                    int n = put_utf8(out + len, out_size - 1 - len, cp);
                    if (n < 0) goto done;
                    len += n;
                    continue;
                }
                default: simple = e;   /* \" \\ \/ */
            }
            if (len >= out_size - 1) goto done;
            out[len++] = simple;
            continue;
        }
        // A plain byte. Don't start a multi-byte character that won't fit.
        int need = 1;
        unsigned char uc = (unsigned char)*v;
        if (uc >= 0xF0) need = 4; else if (uc >= 0xE0) need = 3; else if (uc >= 0xC0) need = 2;
        if (len + need > out_size - 1) goto done;
        for (int i = 0; i < need && *v && *v != '"'; i++) out[len++] = *v++;
    }
done:
    out[len] = '\0';
    return 0;
}

int json_get_int(const char *json, const char *key, int *out) {
    const char *v = find_key(json, key);
    if (!v || is_null(v)) return -1;
    if (*v == '"') v++;
    if (strncmp(v, "true", 4) == 0) { *out = 1; return 0; }
    if (strncmp(v, "false", 5) == 0) { *out = 0; return 0; }
    *out = atoi(v);
    return 0;
}

int json_get_long(const char *json, const char *key, long long *out) {
    const char *v = find_key(json, key);
    if (!v || is_null(v)) return -1;
    if (*v == '"') v++;
    *out = atoll(v);
    return 0;
}

int json_get_bool(const char *json, const char *key, int *out) {
    const char *v = find_key(json, key);
    if (!v || is_null(v)) return -1;
    if (strncmp(v, "true", 4) == 0) { *out = 1; return 0; }
    if (strncmp(v, "false", 5) == 0) { *out = 0; return 0; }
    *out = atoi(v) != 0;
    return 0;
}

int json_has_key(const char *json, const char *key) {
    const char *v = find_key(json, key);
    return v && !is_null(v);
}

int json_get_nested(const char *json, const char *key, const char **start, const char **end) {
    const char *v = find_key(json, key);
    if (!v || (*v != '{' && *v != '[')) return -1;
    *start = v;
    *end = skip_value(v);
    return 0;
}

int json_get_array(const char *json, const char *key, const char **start, const char **end) {
    const char *v = find_key(json, key);
    if (!v || *v != '[') return -1;
    *start = v;
    *end = skip_value(v);
    return 0;
}

int json_array_len(const char *arr_start, const char *arr_end) {
    if (!arr_start || !arr_end || *arr_start != '[') return 0;
    int count = 0;
    const char *p = skip_ws(arr_start + 1);
    while (p < arr_end && *p && *p != ']') {
        p = skip_value(p);
        count++;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
        else break;
    }
    return count;
}

int json_array_get_item(const char *arr_start, const char *arr_end, int index,
                        const char **item_start, const char **item_end) {
    if (!arr_start || !arr_end || *arr_start != '[' || index < 0) return -1;
    int i = 0;
    const char *p = skip_ws(arr_start + 1);
    while (p < arr_end && *p && *p != ']') {
        const char *e = skip_value(p);
        if (i == index) { *item_start = p; *item_end = e; return 0; }
        i++;
        p = skip_ws(e);
        if (*p == ',') p = skip_ws(p + 1);
        else break;
    }
    return -1;
}

char *json_copy(const char *start, const char *end) {
    if (!start || !end || end < start) return NULL;
    size_t n = (size_t)(end - start);
    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, start, n);
    s[n] = '\0';
    return s;
}

char *json_array_item_dup(const char *arr_start, const char *arr_end, int index) {
    const char *s, *e;
    if (json_array_get_item(arr_start, arr_end, index, &s, &e) != 0) return NULL;
    return json_copy(s, e);
}
