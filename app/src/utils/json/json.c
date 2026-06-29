#include "utils/json/json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(webos_json, LOG_LEVEL_INF);

bool json_has_key(const char* json, const char* key) {
  char pattern[32];

  if (snprintk(pattern, sizeof(pattern), "\"%s\"", key) >= sizeof(pattern)) {
    return false;
  }

  return strstr(json, pattern) != NULL;
}

int json_get_string(const char* json, const char* key, char* out, size_t out_len) {
  char pattern[32];
  const char* p;
  size_t i = 0;

  if (snprintk(pattern, sizeof(pattern), "\"%s\"", key) >= sizeof(pattern)) {
    return -EINVAL;
  }

  p = strstr(json, pattern);
  if (p == NULL) {
    return -ENOENT;
  }

  p += strlen(pattern);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  if (*p++ != ':') {
    return -EINVAL;
  }
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  if (*p++ != '"') {
    return -EINVAL;
  }

  while (*p != '\0' && *p != '"') {
    if (i + 1 >= out_len) {
      return -ENOMEM;
    }
    if (*p == '\\') {
      p++;
      if (*p == '\0') {
        return -EINVAL;
      }
      switch (*p) {
        case 'n':
          out[i++] = '\n';
          break;
        case 'r':
          out[i++] = '\r';
          break;
        case 't':
          out[i++] = '\t';
          break;
        default:
          out[i++] = *p;
          break;
      }
    } else {
      out[i++] = *p;
    }
    p++;
  }

  if (*p != '"') {
    return -EINVAL;
  }
  out[i] = '\0';
  return 0;
}

size_t append_json_string(char* dst, size_t dst_len, const char* src, size_t src_len) {
  size_t pos = 0;
  int ret;

  for (size_t i = 0; i < src_len && pos + 1 < dst_len; i++) {
    switch (src[i]) {
      case '\\':
      case '"':
        if (pos + 2 >= dst_len) {
          return pos;
        }
        dst[pos++] = '\\';
        dst[pos++] = src[i];
        break;
      case '\n':
        if (pos + 2 >= dst_len) {
          return pos;
        }
        dst[pos++] = '\\';
        dst[pos++] = 'n';
        break;
      case '\r':
        if (pos + 2 >= dst_len) {
          return pos;
        }
        dst[pos++] = '\\';
        dst[pos++] = 'r';
        break;
      case '\t':
        if (pos + 2 >= dst_len) {
          return pos;
        }
        dst[pos++] = '\\';
        dst[pos++] = 't';
        break;
      default:
        if ((unsigned char)src[i] < ' ') {
          if (pos + 6 >= dst_len) {
            return pos;
          }
          ret = snprintk(dst + pos, dst_len - pos, "\\u%04x", src[i]);
          if (ret < 0 || (size_t)ret >= dst_len - pos) {
            return pos;
          }
          pos += ret;
          break;
        }
        dst[pos++] = src[i];
        break;
    }
  }

  dst[pos] = '\0';
  return pos;
}
