#ifndef SS_JSON_H
#define SS_JSON_H

#include <stddef.h>

// A small reader for the server's JSON answers. Every lookup reads only the
// object's OWN keys: a key with the same name inside a nested object or
// array (a reply inside a comment, a like inside a post) is never matched.
// All functions return 0 when the key was found with a usable value, -1
// otherwise (missing key, or null), and leave *out untouched on -1.

// Copies a string value, decoding its escapes, cut to fit out_size.
int json_get_string(const char *json, const char *key, char *out, int out_size);
int json_get_int(const char *json, const char *key, int *out);
int json_get_long(const char *json, const char *key, long long *out);
int json_get_bool(const char *json, const char *key, int *out);
// 1 when the key is there and not null.
int json_has_key(const char *json, const char *key);

// The array or object value of a key, as [start, end): end is one past the
// closing bracket.
int json_get_array(const char *json, const char *key, const char **start, const char **end);
int json_get_nested(const char *json, const char *key, const char **start, const char **end);

int json_array_len(const char *arr_start, const char *arr_end);
// The index-th item as [item_start, item_end).
int json_array_get_item(const char *arr_start, const char *arr_end, int index,
                        const char **item_start, const char **item_end);

// Heap copies the caller frees: a span, or one array item.
char *json_copy(const char *start, const char *end);
char *json_array_item_dup(const char *arr_start, const char *arr_end, int index);

#endif
