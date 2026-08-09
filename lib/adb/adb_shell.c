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
  bool interactive;
  bool tx_in_flight;
  bool saw_carriage_return;
  bool close_requested;
  bool discard_line;
  uint8_t escape_state;
  size_t command_len;
  size_t input_len;
  size_t input_capacity;
  char command[CONFIG_WEBOS_ADB_SHELL_COMMAND_SIZE];
  uint8_t* input;
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
  size_t remaining;
  size_t chunk_len;
  int ret;

  if (context->tx_in_flight) {
    return 0;
  }

  if (context->interactive) {
    if (context->output_len == 0U) {
      return 0;
    }
    chunk_len = MIN(context->output_len, webos_adb_stream_max_payload(context->stream));
    ret = webos_adb_stream_write(context->stream, context->output, chunk_len);
    if (ret == 0) {
      context->output_len -= chunk_len;
      memmove(context->output, context->output + chunk_len, context->output_len);
      context->tx_in_flight = true;
    }
    return ret;
  }

  remaining = context->output_len - context->output_offset;
  chunk_len = MIN(remaining, webos_adb_stream_max_payload(context->stream));
  if (chunk_len == 0U) {
    LOG_DBG("ADB shell completed rc=%d truncated=%d", context->command_rc, context->truncated);
    webos_adb_stream_close(context->stream);
    return 0;
  }

  ret = webos_adb_stream_write(context->stream, context->output + context->output_offset, chunk_len);
  if (ret == 0) {
    context->output_offset += chunk_len;
    context->tx_in_flight = true;
  }
  return ret;
}

static int adb_shell_open(struct webos_adb_stream* stream, const uint8_t* suffix, size_t suffix_len) {
  static const char prompt[] = CONFIG_SHELL_PROMPT_DUMMY;
  struct adb_shell_context* context;
  char command[CONFIG_WEBOS_ADB_SHELL_COMMAND_SIZE];
  int ret;

  if (suffix == NULL || suffix_len >= sizeof(command) || memchr(suffix, '\0', suffix_len) != NULL) {
    return -EINVAL;
  }
  if (suffix_len != 0U) {
    memcpy(command, suffix, suffix_len);
    command[suffix_len] = '\0';
    if (!command_allowed(command)) {
      LOG_DBG("Rejected unsupported ADB shell command");
      return -EACCES;
    }
  }

  k_mutex_lock(&context_lock, K_FOREVER);
  context = allocate_context(stream);
  k_mutex_unlock(&context_lock);
  if (context == NULL) {
    return -EBUSY;
  }

  if (suffix_len == 0U) {
    context->input = k_malloc(CONFIG_WEBOS_ADB_SHELL_INPUT_SIZE);
    if (context->input == NULL) {
      k_mutex_lock(&context_lock, K_FOREVER);
      memset(context, 0, sizeof(*context));
      k_mutex_unlock(&context_lock);
      return -ENOMEM;
    }
    context->input_capacity = CONFIG_WEBOS_ADB_SHELL_INPUT_SIZE;
    context->interactive = true;
    webos_shell_clear_output();
    context->output_len = MIN(strlen(prompt), sizeof(context->output));
    memcpy(context->output, prompt, context->output_len);
  } else {
    context->command_rc = webos_shell_execute(command, (char*)context->output, sizeof(context->output),
                                              &context->output_len, &context->truncated);
  }

  ret = queue_next_chunk(context);
  if (ret != 0) {
    k_free(context->input);
    k_mutex_lock(&context_lock, K_FOREVER);
    memset(context, 0, sizeof(*context));
    k_mutex_unlock(&context_lock);
  }
  return ret;
}

static void append_output(struct adb_shell_context* context, const char* data, size_t len) {
  size_t available = sizeof(context->output) - context->output_len;
  size_t count = MIN(len, available);

  if (count != 0U) {
    memcpy(context->output + context->output_len, data, count);
    context->output_len += count;
  }
  context->truncated |= count != len;
}

static int execute_interactive_line(struct adb_shell_context* context, bool add_prompt) {
  static const char prompt[] = CONFIG_SHELL_PROMPT_DUMMY;
  size_t captured = 0U;
  size_t available;
  bool truncated = false;

  append_output(context, "\r\n", 2U);
  context->command[context->command_len] = '\0';
  if (strcmp(context->command, "exit") == 0) {
    context->command_len = 0U;
    context->close_requested = true;
    webos_adb_stream_close(context->stream);
    return 0;
  }

  if (context->command_len != 0U) {
    size_t prompt_len = add_prompt ? strlen(prompt) : 0U;

    available = sizeof(context->output) - context->output_len;
    if (available <= prompt_len + 1U) {
      context->truncated = true;
    } else {
      context->command_rc = webos_shell_execute(context->command, (char*)context->output + context->output_len,
                                                available - prompt_len, &captured, &truncated);
      context->output_len += captured;
      context->truncated |= truncated;
    }
  }
  context->command_len = 0U;
  if (add_prompt) {
    append_output(context, prompt, strlen(prompt));
  }
  return 0;
}

static int process_interactive_input(struct adb_shell_context* context) {
  size_t consumed = 0U;
  int ret = 0;

  while (consumed < context->input_len) {
    uint8_t value = context->input[consumed++];

    if (context->escape_state == 1U) {
      if (value == '[' || value == 'O') {
        context->escape_state = 2U;
        continue;
      }
      context->escape_state = 0U;
    } else if (context->escape_state == 2U) {
      if (value >= 0x40U && value <= 0x7eU) {
        context->escape_state = 0U;
      }
      continue;
    }
    if (value == 0x1bU) {
      context->escape_state = 1U;
      continue;
    }

    if (value == '\n' && context->saw_carriage_return) {
      context->saw_carriage_return = false;
      continue;
    }
    context->saw_carriage_return = value == '\r';

    if (context->discard_line) {
      if (value == '\r' || value == '\n') {
        context->discard_line = false;
        append_output(context, "\r\n" CONFIG_SHELL_PROMPT_DUMMY, sizeof("\r\n" CONFIG_SHELL_PROMPT_DUMMY) - 1U);
        break;
      }
      continue;
    }

    if (value == '\r' || value == '\n') {
      ret = execute_interactive_line(context, true);
      if (ret != 0 || context->close_requested || context->output_len != 0U) {
        break;
      }
      continue;
    }
    if (value == 0x04U) {
      if (context->command_len != 0U) {
        ret = execute_interactive_line(context, false);
      }
      context->close_requested = true;
      webos_adb_stream_close(context->stream);
      break;
    }
    if (value == 0x03U) {
      context->command_len = 0U;
      append_output(context, "^C\r\n" CONFIG_SHELL_PROMPT_DUMMY, sizeof("^C\r\n" CONFIG_SHELL_PROMPT_DUMMY) - 1U);
      break;
    }
    if (value == '\b' || value == 0x7fU) {
      if (context->command_len != 0U) {
        --context->command_len;
        append_output(context, "\b \b", 3U);
      }
      continue;
    }
    if (value == '\t') {
      append_output(context, "\a", 1U);
      continue;
    }
    if (value < 0x20U || value > 0x7eU) {
      continue;
    }
    if (context->command_len + 1U >= sizeof(context->command)) {
      context->command_len = 0U;
      context->discard_line = true;
      append_output(context, "\r\nCommand too long", sizeof("\r\nCommand too long") - 1U);
      break;
    }
    context->command[context->command_len++] = (char)value;
    append_output(context, (const char*)&value, 1U);
  }

  context->input_len -= consumed;
  if (context->input_len != 0U && consumed != 0U) {
    memmove(context->input, context->input + consumed, context->input_len);
  }
  if (context->close_requested) {
    context->input_len = 0U;
  }
  return ret;
}

static int adb_shell_write(struct webos_adb_stream* stream, const uint8_t* data, size_t len) {
  struct adb_shell_context* context = find_context(stream);
  int ret;

  if (context == NULL || (data == NULL && len != 0U)) {
    return -EINVAL;
  }
  if (!context->interactive) {
    return len == 0U ? 0 : -ENOTSUP;
  }
  if (len > context->input_capacity - context->input_len) {
    return -ENOSPC;
  }
  if (len != 0U) {
    memcpy(context->input + context->input_len, data, len);
    context->input_len += len;
  }

  ret = 0;
  if (!context->tx_in_flight && context->output_len == 0U) {
    ret = process_interactive_input(context);
  }
  if (ret == 0) {
    ret = queue_next_chunk(context);
  }
  return ret;
}

static void adb_shell_tx_ready(struct webos_adb_stream* stream) {
  struct adb_shell_context* context;

  k_mutex_lock(&context_lock, K_FOREVER);
  context = find_context(stream);
  k_mutex_unlock(&context_lock);
  if (context == NULL) {
    return;
  }

  context->tx_in_flight = false;
  if (context->interactive && context->output_len == 0U && context->input_len != 0U && !context->close_requested &&
      process_interactive_input(context) != 0) {
    LOG_WRN("Failed to process interactive ADB shell input");
    webos_adb_stream_close(stream);
    return;
  }
  if (queue_next_chunk(context) != 0) {
    LOG_WRN("Failed to continue ADB shell output");
    webos_adb_stream_close(stream);
  }
}

static void adb_shell_close(struct webos_adb_stream* stream) {
  struct adb_shell_context* context;

  k_mutex_lock(&context_lock, K_FOREVER);
  context = find_context(stream);
  if (context != NULL) {
    if (context->interactive) {
      webos_shell_clear_output();
    }
    k_free(context->input);
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
