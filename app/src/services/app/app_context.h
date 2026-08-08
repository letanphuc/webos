#ifndef WEBOS_SERVICES_APP_CONTEXT_H
#define WEBOS_SERVICES_APP_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "webos.h"

#define WEBOS_APP_MAX_ID 48
#define WEBOS_APP_MAX_GRANTS 8

struct webos_app_context {
  char id[WEBOS_APP_MAX_ID];
  const char* version;
  char storage_root[WEBOS_ABI_MAX_PATH_BYTES + 1];
  const char* path_grants[WEBOS_APP_MAX_GRANTS];
  size_t path_grant_count;
  const char* http_origins[WEBOS_APP_MAX_GRANTS];
  size_t http_origin_count;
  uint32_t max_io_bytes;
  bool ready;
  int64_t ready_at_ms;
  int64_t heartbeat_at_ms;
};

int webos_app_context_init(struct webos_app_context* context, const char* id, const char* version);
bool webos_app_path_allowed(const struct webos_app_context* context, const char* path);
bool webos_app_url_allowed(const struct webos_app_context* context, const char* url, size_t url_len);
typedef bool (*webos_memory_validator_t)(const void* data, uint32_t length, void* user_data);
int webos_abi_validate_buffer(const struct webos_app_context* context, const void* data, uint32_t length,
                              webos_memory_validator_t validator, void* user_data);
int webos_abi_check_version(uint32_t requested_version);
int32_t webos_abi_map_errno(int error);
void webos_app_mark_ready(struct webos_app_context* context, int64_t now_ms);
void webos_app_mark_heartbeat(struct webos_app_context* context, int64_t now_ms);

#endif
