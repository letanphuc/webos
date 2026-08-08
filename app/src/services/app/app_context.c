#include "services/app/app_context.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static bool valid_component(const char* value) {
  size_t length;
  if (value == NULL || (length = strlen(value)) == 0 || length >= WEBOS_APP_MAX_ID) return false;
  for (size_t i = 0; i < length; ++i) {
    char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

static bool prefix_boundary(const char* value, const char* prefix) {
  size_t n = strlen(prefix);
  return n > 0 && strncmp(value, prefix, n) == 0 && (value[n] == '\0' || value[n] == '/' || prefix[n - 1] == '/');
}

static bool valid_utf8(const char* value, size_t length) {
  const uint8_t* bytes = (const uint8_t*)value;
  size_t i = 0;

  while (i < length) {
    uint8_t first = bytes[i++];
    uint32_t codepoint;
    size_t continuation;
    if (first < 0x80) continue;
    if (first >= 0xc2 && first <= 0xdf) {
      codepoint = first & 0x1f;
      continuation = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      codepoint = first & 0x0f;
      continuation = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codepoint = first & 0x07;
      continuation = 3;
    } else {
      return false;
    }
    if (continuation > length - i) return false;
    while (continuation-- > 0) {
      uint8_t next = bytes[i++];
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff || (codepoint < 0x800 && first >= 0xe0) ||
        (codepoint < 0x10000 && first >= 0xf0))
      return false;
  }
  return true;
}

static bool valid_path(const char* path) {
  size_t n;
  if (path == NULL || path[0] != '/' || (n = strnlen(path, WEBOS_ABI_MAX_PATH_BYTES + 1)) == 0 ||
      n > WEBOS_ABI_MAX_PATH_BYTES || path[n - 1] == '/')
    return false;
  if (!valid_utf8(path, n) || strstr(path, "//") != NULL) return false;
  for (const char* p = path; *p != '\0'; ++p) {
    if ((unsigned char)*p < 0x20 ||
        (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') && (p[2] == '\0' || p[2] == '/')))
      return false;
  }
  return true;
}

int webos_app_context_init(struct webos_app_context* context, const char* id, const char* version) {
  int ret;
  if (context == NULL || !valid_component(id) || version == NULL || version[0] == '\0') return WEBOS_INVALID;
  memset(context, 0, sizeof(*context));
  memcpy(context->id, id, strlen(id) + 1);
  context->version = version;
  context->max_io_bytes = WEBOS_ABI_MAX_IO_BYTES;
  ret = snprintf(context->storage_root, sizeof(context->storage_root), "/STORAGE:/apps/%s/data", id);
  return ret < 0 || (size_t)ret >= sizeof(context->storage_root) ? WEBOS_TOO_LARGE : WEBOS_OK;
}

bool webos_app_path_allowed(const struct webos_app_context* context, const char* path) {
  if (context == NULL || !valid_path(path)) return false;
  if (prefix_boundary(path, context->storage_root)) return true;
  for (size_t i = 0; i < context->path_grant_count && i < WEBOS_APP_MAX_GRANTS; ++i) {
    if (context->path_grants[i] != NULL && prefix_boundary(path, context->path_grants[i])) return true;
  }
  return false;
}

bool webos_app_url_allowed(const struct webos_app_context* context, const char* url, size_t url_len) {
  if (context == NULL || url == NULL || url_len == 0 || memchr(url, '\0', url_len) != NULL) return false;
  for (size_t i = 0; i < context->http_origin_count && i < WEBOS_APP_MAX_GRANTS; ++i) {
    const char* origin = context->http_origins[i];
    size_t n = origin == NULL ? 0 : strlen(origin);
    if (n == 1 && origin[0] == '*' &&
        ((url_len > 7 && memcmp(url, "http://", 7) == 0) || (url_len > 8 && memcmp(url, "https://", 8) == 0)))
      return true;
    if (n && url_len >= n && memcmp(url, origin, n) == 0 &&
        (url_len == n || url[n] == '/' || url[n] == '?' || url[n] == '#'))
      return true;
  }
  return false;
}

int webos_abi_validate_buffer(const struct webos_app_context* context, const void* data, uint32_t length,
                              webos_memory_validator_t validator, void* user_data) {
  if (context == NULL || validator == NULL) return WEBOS_INVALID;
  if (length > context->max_io_bytes) return WEBOS_TOO_LARGE;
  if (length == 0) return WEBOS_OK;
  return data != NULL && validator(data, length, user_data) ? WEBOS_OK : WEBOS_INVALID;
}

int webos_abi_check_version(uint32_t requested_version) {
  uint32_t major = requested_version >> 16;
  uint32_t minor = requested_version & 0xffffu;
  return major == WEBOS_ABI_MAJOR && minor <= WEBOS_ABI_MINOR ? WEBOS_OK : WEBOS_UNSUPPORTED;
}

int32_t webos_abi_map_errno(int error) {
  if (error >= 0) return error;
  switch (-error) {
    case EINVAL:
    case EFAULT:
    case ENAMETOOLONG:
      return WEBOS_INVALID;
    case EACCES:
    case EPERM:
      return WEBOS_DENIED;
    case ENOENT:
      return WEBOS_NOT_FOUND;
    case EFBIG:
    case ENOSPC:
      return WEBOS_TOO_LARGE;
    case EBUSY:
    case EAGAIN:
      return WEBOS_BUSY;
    case ENOTSUP:
      return WEBOS_UNSUPPORTED;
    default:
      return WEBOS_IO;
  }
}

void webos_app_mark_ready(struct webos_app_context* context, int64_t now_ms) {
  if (context != NULL) {
    context->ready = true;
    context->ready_at_ms = now_ms;
  }
}
void webos_app_mark_heartbeat(struct webos_app_context* context, int64_t now_ms) {
  if (context != NULL) context->heartbeat_at_ms = now_ms;
}
