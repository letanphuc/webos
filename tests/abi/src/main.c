#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "services/app/app_context.h"

static bool range_validator(const void* data, uint32_t length, void* user_data) {
  const uintptr_t start = (uintptr_t)user_data;
  const uintptr_t pointer = (uintptr_t)data;
  return pointer >= start && length <= 64 && pointer - start <= 64 - length;
}

ZTEST(webos_abi, test_version_compatibility) {
  zassert_equal(webos_abi_check_version((1u << 16) | 0u), WEBOS_OK);
  zassert_equal(webos_abi_check_version((1u << 16) | 1u), WEBOS_UNSUPPORTED);
  zassert_equal(webos_abi_check_version((2u << 16) | 0u), WEBOS_UNSUPPORTED);
  zassert_equal(webos_abi_check_version(0), WEBOS_UNSUPPORTED);
}

ZTEST(webos_abi, test_malformed_pointer_and_oversized_buffer) {
  struct webos_app_context context;
  uint8_t memory[64];
  zassert_ok(webos_app_context_init(&context, "test-app", "1.0.0"));
  zassert_equal(webos_abi_validate_buffer(&context, NULL, 1, range_validator, memory), WEBOS_INVALID);
  zassert_equal(webos_abi_validate_buffer(&context, memory + 63, 2, range_validator, memory), WEBOS_INVALID);
  zassert_equal(webos_abi_validate_buffer(&context, (void*)(UINTPTR_MAX - 1), 4, range_validator, memory),
                WEBOS_INVALID);
  zassert_equal(webos_abi_validate_buffer(&context, memory, context.max_io_bytes + 1, range_validator, memory),
                WEBOS_TOO_LARGE);
  zassert_ok(webos_abi_validate_buffer(&context, NULL, 0, range_validator, memory));
}

ZTEST(webos_abi, test_paths_are_app_scoped) {
  struct webos_app_context context;
  const char* grants[] = {"/dev/led/status"};
  zassert_ok(webos_app_context_init(&context, "room-light", "1.0.0"));
  context.path_grants[0] = grants[0];
  context.path_grant_count = 1;
  zassert_true(webos_app_path_allowed(&context, "/STORAGE:/apps/room-light/data/config.bin"));
  zassert_true(webos_app_path_allowed(&context, "/dev/led/status"));
  zassert_false(webos_app_path_allowed(&context, "/dev/led/status-extra"));
  zassert_false(webos_app_path_allowed(&context, "/STORAGE:/apps/other/data/config.bin"));
  zassert_false(webos_app_path_allowed(&context, "/STORAGE:/apps/room-light/data/../secret"));
  zassert_false(webos_app_path_allowed(&context, "/STORAGE:/apps/room-light//data"));
  zassert_false(webos_app_path_allowed(&context, "relative"));
  zassert_false(webos_app_path_allowed(&context, "/STORAGE:/apps/room-light/data/\xc0\xaf"));
}

ZTEST(webos_abi, test_http_origin_policy_and_lifecycle) {
  struct webos_app_context context;
  zassert_ok(webos_app_context_init(&context, "net-app", "1.0.0"));
  context.http_origins[0] = "https://api.example.com";
  context.http_origin_count = 1;
  zassert_true(webos_app_url_allowed(&context, "https://api.example.com/v1", strlen("https://api.example.com/v1")));
  zassert_false(
      webos_app_url_allowed(&context, "https://api.example.com.evil/v1", strlen("https://api.example.com.evil/v1")));
  context.http_origins[0] = "*";
  zassert_true(webos_app_url_allowed(&context, "https://other.example/v1", strlen("https://other.example/v1")));
  zassert_false(webos_app_url_allowed(&context, "file:///secret", strlen("file:///secret")));
  context.path_grants[0] = "";
  zassert_false(webos_app_path_allowed(&context, "/dev/led/status"));
  webos_app_mark_ready(&context, 10);
  webos_app_mark_heartbeat(&context, 20);
  zassert_true(context.ready);
  zassert_equal(context.ready_at_ms, 10);
  zassert_equal(context.heartbeat_at_ms, 20);
}

ZTEST(webos_abi, test_errno_mapping_is_stable) {
  zassert_equal(webos_abi_map_errno(-EACCES), WEBOS_DENIED);
  zassert_equal(webos_abi_map_errno(-ENOENT), WEBOS_NOT_FOUND);
  zassert_equal(webos_abi_map_errno(-ENOSPC), WEBOS_TOO_LARGE);
  zassert_equal(webos_abi_map_errno(-EIO), WEBOS_IO);
  zassert_equal(webos_abi_map_errno(7), 7);
}
ZTEST_SUITE(webos_abi, NULL, NULL, NULL, NULL, NULL);
