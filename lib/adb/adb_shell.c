/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <webos/shell_exec.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "adb_service.h"

LOG_MODULE_DECLARE(webos_adb, LOG_LEVEL_INF);

struct adb_shell_context {
  struct webos_adb_stream* stream;
  size_t output_len;
  size_t output_offset;
  int command_rc;
  bool truncated;
  uint8_t output[CONFIG_WEBOS_ADB_SHELL_OUTPUT_SIZE];
};

static K_MUTEX_DEFINE(context_lock);
static struct adb_shell_context contexts[CONFIG_WEBOS_ADB_SHELL_MAX_STREAMS];

static bool safe_path_char(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
         value == '/' || value == ':' || value == '.' || value == '_' || value == '-';
}

static bool safe_path(const char* path, size_t len, bool storage_only) {
  static const char storage[] = "/STORAGE:";
  const size_t storage_len = sizeof(storage) - 1U;
  size_t first_segment;

  if (storage_only) {
    if (len <= storage_len || memcmp(path, storage, storage_len) != 0 || path[storage_len] != '/') {
      return false;
    }
    first_segment = storage_len + 1U;
  } else if (len == storage_len && memcmp(path, storage, storage_len) == 0) {
    return true;
  } else if (len > storage_len && memcmp(path, storage, storage_len) == 0 && path[storage_len] == '/') {
    first_segment = storage_len + 1U;
  } else if (len >= 4U && memcmp(path, "/dev", 4U) == 0 && (len == 4U || path[4] == '/')) {
    first_segment = len == 4U ? len : 5U;
  } else {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    if (!safe_path_char(path[i])) {
      return false;
    }
  }
  for (size_t start = first_segment; start < len;) {
    size_t end = start;

    while (end < len && path[end] != '/') {
      ++end;
    }
    if (end == start || (end - start == 1U && path[start] == '.') ||
        (end - start == 2U && path[start] == '.' && path[start + 1U] == '.')) {
      return false;
    }
    start = end + 1U;
  }
  return path[len - 1U] != '/';
}

static bool read_command_allowed(const char* arguments) {
  const char* separator = strchr(arguments, ' ');
  char* end;
  unsigned long count;
  unsigned long offset;

  if (separator == NULL || !safe_path(arguments, (size_t)(separator - arguments), true)) {
    return false;
  }
  while (*separator == ' ') {
    ++separator;
  }
  if (*separator == '\0') {
    return false;
  }

  count = strtoul(separator, &end, 10);
  if (end == separator || count == 0UL || count > 256UL) {
    return false;
  }
  while (*end == ' ') {
    ++end;
  }
  if (*end == '\0') {
    return true;
  }

  offset = strtoul(end, &end, 10);
  if (offset > UINT32_MAX) {
    return false;
  }
  while (*end == ' ') {
    ++end;
  }
  return *end == '\0';
}

static bool command_allowed(const char* command) {
  const char* path;

  if (strcmp(command, "kernel uptime") == 0 || strcmp(command, "fs ls") == 0) {
    return true;
  }
  if (strncmp(command, "fs read ", 8) == 0) {
    return read_command_allowed(command + 8);
  }
  if (strncmp(command, "fs ls ", 6) != 0) {
    return false;
  }

  path = command + 6;
  return safe_path(path, strlen(path), false);
}

static struct adb_shell_context* find_context(struct webos_adb_stream* stream) {
  for (size_t i = 0; i < ARRAY_SIZE(contexts); ++i) {
    if (contexts[i].stream == stream) {
      return &contexts[i];
    }
  }
  return NULL;
}

static struct adb_shell_context* allocate_context(struct webos_adb_stream* stream) {
  for (size_t i = 0; i < ARRAY_SIZE(contexts); ++i) {
    if (contexts[i].stream == NULL) {
      memset(&contexts[i], 0, sizeof(contexts[i]));
      contexts[i].stream = stream;
      return &contexts[i];
    }
  }
  return NULL;
}

static int queue_next_chunk(struct adb_shell_context* context) {
  size_t remaining = context->output_len - context->output_offset;
  size_t chunk_len = MIN(remaining, webos_adb_stream_max_payload(context->stream));
  int ret;

  if (chunk_len == 0U) {
    LOG_DBG("ADB shell completed rc=%d truncated=%d", context->command_rc, context->truncated);
    webos_adb_stream_close(context->stream);
    return 0;
  }

  ret = webos_adb_stream_write(context->stream, context->output + context->output_offset, chunk_len);
  if (ret == 0) {
    context->output_offset += chunk_len;
  }
  return ret;
}

static int adb_shell_open(struct webos_adb_stream* stream, const uint8_t* suffix, size_t suffix_len) {
  struct adb_shell_context* context;
  char command[CONFIG_WEBOS_ADB_SHELL_COMMAND_SIZE];
  int ret;

  if (suffix == NULL || suffix_len == 0U || suffix_len >= sizeof(command) || memchr(suffix, '\0', suffix_len) != NULL) {
    return -EINVAL;
  }
  memcpy(command, suffix, suffix_len);
  command[suffix_len] = '\0';
  if (!command_allowed(command)) {
    LOG_DBG("Rejected unsupported ADB shell command");
    return -EACCES;
  }

  k_mutex_lock(&context_lock, K_FOREVER);
  context = allocate_context(stream);
  k_mutex_unlock(&context_lock);
  if (context == NULL) {
    return -EBUSY;
  }

  context->command_rc = webos_shell_execute(command, (char*)context->output, sizeof(context->output),
                                            &context->output_len, &context->truncated);
  ret = queue_next_chunk(context);
  if (ret != 0) {
    k_mutex_lock(&context_lock, K_FOREVER);
    memset(context, 0, sizeof(*context));
    k_mutex_unlock(&context_lock);
  }
  return ret;
}

static int adb_shell_write(struct webos_adb_stream* stream, const uint8_t* data, size_t len) {
  ARG_UNUSED(stream);
  ARG_UNUSED(data);
  return len == 0U ? 0 : -ENOTSUP;
}

static void adb_shell_tx_ready(struct webos_adb_stream* stream) {
  struct adb_shell_context* context;

  k_mutex_lock(&context_lock, K_FOREVER);
  context = find_context(stream);
  k_mutex_unlock(&context_lock);
  if (context != NULL && queue_next_chunk(context) != 0) {
    LOG_WRN("Failed to continue ADB shell output");
    webos_adb_stream_close(stream);
  }
}

static void adb_shell_close(struct webos_adb_stream* stream) {
  struct adb_shell_context* context;

  k_mutex_lock(&context_lock, K_FOREVER);
  context = find_context(stream);
  if (context != NULL) {
    memset(context, 0, sizeof(*context));
  }
  k_mutex_unlock(&context_lock);
}

const struct webos_adb_service_ops webos_adb_shell_service_ops = {
    .open = adb_shell_open,
    .write = adb_shell_write,
    .tx_ready = adb_shell_tx_ready,
    .close = adb_shell_close,
};
