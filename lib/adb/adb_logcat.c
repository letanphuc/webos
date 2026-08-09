/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <webos/log_buffer.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "adb_service.h"

struct adb_logcat_context {
  struct webos_adb_stream* stream;
  uint8_t* ring;
  size_t ring_capacity;
  size_t ring_head;
  size_t ring_len;
  uint32_t stream_generation;
  bool active;
  bool sending;
  bool drain;
  uint8_t tx_chunk[CONFIG_WEBOS_ADB_TX_PAYLOAD_SIZE];
};

static K_MUTEX_DEFINE(logcat_lock);
static struct adb_logcat_context logcat;

static void logcat_work_handler(struct k_work* work);
static K_WORK_DEFINE(logcat_work, logcat_work_handler);

static bool contains_bytes(const uint8_t* data, size_t len, const char* token) {
  size_t token_len = strlen(token);

  if (token_len > len) {
    return false;
  }
  for (size_t i = 0; i <= len - token_len; ++i) {
    if (memcmp(data + i, token, token_len) == 0) {
      return true;
    }
  }
  return false;
}

bool webos_adb_logcat_matches(const uint8_t* suffix, size_t suffix_len) {
  if (suffix == NULL || suffix_len == 0U || suffix_len >= CONFIG_WEBOS_ADB_SHELL_COMMAND_SIZE ||
      memchr(suffix, '\0', suffix_len) != NULL || memchr(suffix, '\n', suffix_len) != NULL ||
      memchr(suffix, '\r', suffix_len) != NULL) {
    return false;
  }

  if (suffix_len == strlen("logcat") && memcmp(suffix, "logcat", suffix_len) == 0) {
    return true;
  }
  if (suffix_len > strlen("logcat ") && memcmp(suffix, "logcat ", strlen("logcat ")) == 0) {
    return true;
  }
  return contains_bytes(suffix, suffix_len, "; exec logcat");
}

static bool option_requested(const uint8_t* suffix, size_t suffix_len, const char* quoted, const char* plain,
                             const char* long_option) {
  return contains_bytes(suffix, suffix_len, quoted) || contains_bytes(suffix, suffix_len, plain) ||
         contains_bytes(suffix, suffix_len, long_option);
}

static bool drain_requested(const uint8_t* suffix, size_t suffix_len) {
  return option_requested(suffix, suffix_len, "'-d'", " -d", "--dump") ||
         option_requested(suffix, suffix_len, "'-t'", " -t", "--tail");
}

static bool clear_requested(const uint8_t* suffix, size_t suffix_len) {
  return option_requested(suffix, suffix_len, "'-c'", " -c", "--clear");
}

static void ring_push(struct adb_logcat_context* context, const uint8_t* data, size_t len) {
  size_t tail;
  size_t first;

  if (len >= context->ring_capacity) {
    data += len - context->ring_capacity;
    len = context->ring_capacity;
    context->ring_head = 0U;
    context->ring_len = 0U;
  }
  if (context->ring_len + len > context->ring_capacity) {
    size_t drop = context->ring_len + len - context->ring_capacity;

    context->ring_head = (context->ring_head + drop) % context->ring_capacity;
    context->ring_len -= drop;
  }

  tail = (context->ring_head + context->ring_len) % context->ring_capacity;
  first = MIN(len, context->ring_capacity - tail);
  memcpy(context->ring + tail, data, first);
  if (first != len) {
    memcpy(context->ring, data + first, len - first);
  }
  context->ring_len += len;
}

static void live_log_listener(const char* data, size_t len, void* user_data) {
  struct adb_logcat_context* context = user_data;

  k_mutex_lock(&logcat_lock, K_FOREVER);
  if (context->active && !context->drain) {
    ring_push(context, (const uint8_t*)data, len);
  }
  k_mutex_unlock(&logcat_lock);
  (void)k_work_submit(&logcat_work);
}

static int queue_next(void) {
  struct webos_adb_stream* stream = NULL;
  size_t chunk_len = 0U;
  uint32_t generation = 0U;
  bool close_stream = false;
  int ret = 0;

  k_mutex_lock(&logcat_lock, K_FOREVER);
  if (!logcat.active || logcat.sending) {
    k_mutex_unlock(&logcat_lock);
    return 0;
  }

  if (logcat.ring_len == 0U) {
    close_stream = logcat.drain;
    stream = logcat.stream;
    generation = logcat.stream_generation;
  } else {
    size_t first;

    stream = logcat.stream;
    generation = logcat.stream_generation;
    chunk_len = MIN(logcat.ring_len, webos_adb_stream_max_payload(stream));
    first = MIN(chunk_len, logcat.ring_capacity - logcat.ring_head);
    memcpy(logcat.tx_chunk, logcat.ring + logcat.ring_head, first);
    if (first != chunk_len) {
      memcpy(logcat.tx_chunk + first, logcat.ring, chunk_len - first);
    }
    logcat.ring_head = (logcat.ring_head + chunk_len) % logcat.ring_capacity;
    logcat.ring_len -= chunk_len;
    logcat.sending = true;
  }
  k_mutex_unlock(&logcat_lock);

  if (close_stream) {
    webos_adb_stream_close_generation(stream, generation);
  } else if (chunk_len != 0U) {
    ret = webos_adb_stream_write_generation(stream, generation, logcat.tx_chunk, chunk_len);
    if (ret != 0) {
      bool current;

      k_mutex_lock(&logcat_lock, K_FOREVER);
      current = logcat.active && logcat.stream_generation == generation;
      if (current) {
        logcat.sending = false;
      }
      k_mutex_unlock(&logcat_lock);
      if (current) {
        webos_adb_stream_close_generation(stream, generation);
      }
    }
  }
  return ret;
}

static void logcat_work_handler(struct k_work* work) {
  ARG_UNUSED(work);
  (void)queue_next();
}

static int adb_logcat_open(struct webos_adb_stream* stream, const uint8_t* suffix, size_t suffix_len) {
  bool clear;
  bool drain;
  size_t snapshot_len = 0U;
  int ret = 0;

  if (!webos_adb_logcat_matches(suffix, suffix_len)) {
    return -EINVAL;
  }
  clear = clear_requested(suffix, suffix_len);
  drain = clear || drain_requested(suffix, suffix_len);

  k_mutex_lock(&logcat_lock, K_FOREVER);
  if (logcat.active) {
    ret = -EBUSY;
    goto out;
  }
  if (logcat.ring == NULL) {
    logcat.ring = k_malloc(CONFIG_WEBOS_ADB_LOGCAT_BUFFER_SIZE);
    if (logcat.ring == NULL) {
      ret = -ENOMEM;
      goto out;
    }
    logcat.ring_capacity = CONFIG_WEBOS_ADB_LOGCAT_BUFFER_SIZE;
  }

  logcat.stream = stream;
  logcat.stream_generation = webos_adb_stream_generation(stream);
  logcat.ring_head = 0U;
  logcat.ring_len = 0U;
  logcat.sending = false;
  logcat.drain = drain;

  if (clear) {
    log_buffer_clear();
  } else if (drain) {
    snapshot_len = log_buffer_read((char*)logcat.ring, logcat.ring_capacity);
  } else {
    ret = log_buffer_snapshot_and_set_listener(live_log_listener, &logcat, (char*)logcat.ring, logcat.ring_capacity,
                                               &snapshot_len);
    if (ret != 0) {
      logcat.stream = NULL;
      logcat.stream_generation = 0U;
      goto out;
    }
  }
  logcat.ring_len = snapshot_len;
  logcat.active = true;

out:
  k_mutex_unlock(&logcat_lock);
  return ret == 0 ? queue_next() : ret;
}

static int adb_logcat_write(struct webos_adb_stream* stream, const uint8_t* data, size_t len) {
  ARG_UNUSED(stream);
  ARG_UNUSED(data);
  return len == 0U ? 0 : -ENOTSUP;
}

static void adb_logcat_tx_ready(struct webos_adb_stream* stream) {
  bool current;

  k_mutex_lock(&logcat_lock, K_FOREVER);
  current = logcat.active && logcat.stream == stream;
  if (current) {
    logcat.sending = false;
  }
  k_mutex_unlock(&logcat_lock);
  if (current) {
    (void)queue_next();
  }
}

static void adb_logcat_close(struct webos_adb_stream* stream) {
  ARG_UNUSED(stream);

  k_mutex_lock(&logcat_lock, K_FOREVER);
  logcat.active = false;
  logcat.sending = false;
  logcat.stream = NULL;
  logcat.stream_generation = 0U;
  logcat.ring_head = 0U;
  logcat.ring_len = 0U;
  k_mutex_unlock(&logcat_lock);
  log_buffer_clear_listener(live_log_listener, &logcat);
}

const struct webos_adb_service_ops webos_adb_logcat_service_ops = {
    .open = adb_logcat_open,
    .write = adb_logcat_write,
    .tx_ready = adb_logcat_tx_ready,
    .close = adb_logcat_close,
};
