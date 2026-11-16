// Unit tests for the JSON reader (lib/ss_json.c). Built and run by `make unit`.
#include "ss_json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0, passes = 0;
#define CHECK(name, cond) do { if (cond) passes++; else { fails++; printf("FAIL  %s\n", name); } } while (0)

int main(void) {
    char s[256];
    int n = -7;

    // A nested object (a reply) has the same key BEFORE the outer one.
    const char *comment = "{\"replies\":[{\"id\":99,\"text\":\"inner\",\"user_email\":\"in@x\"}],"
                          "\"meta\":{\"id\":55},\"id\":12,\"text\":\"outer\",\"user_email\":\"out@x\"}";
    CHECK("outer id, not the reply's", json_get_int(comment, "id", &n) == 0 && n == 12);
    CHECK("outer text", json_get_string(comment, "text", s, sizeof s) == 0 && strcmp(s, "outer") == 0);
    CHECK("outer email", json_get_string(comment, "user_email", s, sizeof s) == 0 && strcmp(s, "out@x") == 0);

    // A key name that appears inside a string value is not a key.
    const char *tricky = "{\"text\":\"say \\\"id\\\": 5\",\"id\":3}";
    CHECK("key inside a string is ignored", json_get_int(tricky, "id", &n) == 0 && n == 3);
    CHECK("escaped quotes decoded", json_get_string(tricky, "text", s, sizeof s) == 0 && strcmp(s, "say \"id\": 5") == 0);

    // Escapes and unicode.
    const char *esc = "{\"a\":\"line\\nnext\\t\\/\\\\\",\"b\":\"caf\\u00e9 \\ud83d\\ude00\",\"c\":\"señal\"}";
    CHECK("\\n \\t \\/ \\\\", json_get_string(esc, "a", s, sizeof s) == 0 && strcmp(s, "line\nnext\t/\\") == 0);
    CHECK("\\u escapes and surrogate pairs to UTF-8", json_get_string(esc, "b", s, sizeof s) == 0 && strcmp(s, "caf\xc3\xa9 \xf0\x9f\x98\x80") == 0);
    CHECK("raw UTF-8 kept", json_get_string(esc, "c", s, sizeof s) == 0 && strcmp(s, "se\xc3\xb1" "al") == 0);
    char small[4];
    CHECK("cut at a whole character", json_get_string(esc, "c", small, sizeof small) == 0 && strcmp(small, "se") == 0);

    // null and missing.
    const char *nul = "{\"x\":null,\"y\":true,\"z\":false,\"w\":\"7\"}";
    n = -7;
    CHECK("null int leaves out alone", json_get_int(nul, "x", &n) == -1 && n == -7);
    CHECK("null string is -1", json_get_string(nul, "x", s, sizeof s) == -1);
    CHECK("missing key is -1", json_get_int(nul, "nope", &n) == -1);
    CHECK("bools", json_get_bool(nul, "y", &n) == 0 && n == 1 && json_get_bool(nul, "z", &n) == 0 && n == 0);
    CHECK("quoted number", json_get_int(nul, "w", &n) == 0 && n == 7);
    CHECK("has_key", json_has_key(nul, "y") && !json_has_key(nul, "x") && !json_has_key(nul, "q"));

    // Arrays: items of every kind, nested ones included.
    const char *arr = "{\"items\":[{\"id\":1,\"inner\":[1,2,{\"id\":9}]},\"two\",3,[4,5],{\"id\":6}],\"after\":1}";
    const char *as, *ae;
    CHECK("get array", json_get_array(arr, "items", &as, &ae) == 0 && *as == '[' && ae[-1] == ']');
    CHECK("array length", json_array_len(as, ae) == 5);
    char *it = json_array_item_dup(as, ae, 0);
    CHECK("first item's own id", it && json_get_int(it, "id", &n) == 0 && n == 1);
    free(it);
    it = json_array_item_dup(as, ae, 1);
    CHECK("string item", it && strcmp(it, "\"two\"") == 0);
    free(it);
    it = json_array_item_dup(as, ae, 4);
    CHECK("last item", it && json_get_int(it, "id", &n) == 0 && n == 6);
    free(it);
    CHECK("past the end", json_array_item_dup(as, ae, 5) == NULL);
    CHECK("empty array", json_get_array("{\"e\":[]}", "e", &as, &ae) == 0 && json_array_len(as, ae) == 0);
    CHECK("key after the array", json_get_int(arr, "after", &n) == 0 && n == 1);

    // A big answer: 25 posts of 5,000 characters each.
    size_t cap = 25 * 6000 + 64;
    char *big = malloc(cap);
    size_t len = 0;
    len += sprintf(big + len, "{\"posts\":[");
    for (int i = 0; i < 25; i++) {
        len += sprintf(big + len, "%s{\"id\":\"p%d\",\"text\":\"", i ? "," : "", i);
        for (int k = 0; k < 5000; k++) big[len++] = (char)('a' + (k % 26));
        len += sprintf(big + len, "\"}");
    }
    len += sprintf(big + len, "],\"hasMore\":true}");
    big[len] = '\0';
    CHECK("big: 25 items", json_get_array(big, "posts", &as, &ae) == 0 && json_array_len(as, ae) == 25);
    it = json_array_item_dup(as, ae, 24);
    char *text = malloc(6000);
    CHECK("big: last item's full text", it && json_get_string(it, "text", text, 6000) == 0 && strlen(text) == 5000);
    CHECK("big: key after the posts", json_get_bool(big, "hasMore", &n) == 0 && n == 1);
    free(it); free(text); free(big);

    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
