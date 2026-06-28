#ifndef WEBOS_UTILS_JSON_H
#define WEBOS_UTILS_JSON_H

#include <stddef.h>

int json_get_string(const char *json, const char *key, char *out, size_t out_len);
size_t append_json_string(char *dst, size_t dst_len, const char *src, size_t src_len);

#endif
