#include <errno.h>
#include <string.h>
#include <webos/log_buffer.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_output.h>

static char log_buf[CONFIG_WEBOS_LOG_BUFFER_SIZE];
static size_t log_head;
static size_t log_len;
static webos_log_buffer_listener_t log_listener;
static void* log_listener_context;
static K_MUTEX_DEFINE(log_lock);

static uint8_t output_buf[CONFIG_WEBOS_LOG_BACKEND_BUF_SIZE];
static uint32_t log_format_current = CONFIG_WEBOS_LOG_BACKEND_OUTPUT_DEFAULT;

void log_buffer_init(void) {}

static size_t copy_log_locked(char* dst, size_t dst_len) {
  size_t count;
  size_t first;

  if (dst == NULL || dst_len == 0U) {
    return 0U;
  }
  count = MIN(log_len, dst_len - 1U);
  first = MIN(count, CONFIG_WEBOS_LOG_BUFFER_SIZE - log_head);
  memcpy(dst, log_buf + log_head, first);
  if (first != count) {
    memcpy(dst + first, log_buf, count - first);
  }
  dst[count] = '\0';
  return count;
}

void log_buffer_put(const char* msg, size_t len) {
  webos_log_buffer_listener_t listener;
  void* listener_context;
  size_t tail;
  size_t first;

  if (msg == NULL || len == 0U) {
    return;
  }

  k_mutex_lock(&log_lock, K_FOREVER);
  if (len >= CONFIG_WEBOS_LOG_BUFFER_SIZE) {
    msg += len - CONFIG_WEBOS_LOG_BUFFER_SIZE;
    len = CONFIG_WEBOS_LOG_BUFFER_SIZE;
    log_head = 0U;
    log_len = 0U;
  }
  if (log_len + len > CONFIG_WEBOS_LOG_BUFFER_SIZE) {
    size_t drop = log_len + len - CONFIG_WEBOS_LOG_BUFFER_SIZE;

    log_head = (log_head + drop) % CONFIG_WEBOS_LOG_BUFFER_SIZE;
    log_len -= drop;
  }
  tail = (log_head + log_len) % CONFIG_WEBOS_LOG_BUFFER_SIZE;
  first = MIN(len, CONFIG_WEBOS_LOG_BUFFER_SIZE - tail);
  memcpy(log_buf + tail, msg, first);
  if (first != len) {
    memcpy(log_buf, msg + first, len - first);
  }
  log_len += len;
  listener = log_listener;
  listener_context = log_listener_context;
  k_mutex_unlock(&log_lock);

  if (listener != NULL) {
    listener(msg, len, listener_context);
  }
}

size_t log_buffer_read(char* dst, size_t dst_len) {
  size_t data_len;

  k_mutex_lock(&log_lock, K_FOREVER);
  data_len = copy_log_locked(dst, dst_len);
  k_mutex_unlock(&log_lock);
  return data_len;
}

void log_buffer_clear(void) {
  k_mutex_lock(&log_lock, K_FOREVER);
  log_head = 0U;
  log_len = 0U;
  k_mutex_unlock(&log_lock);
}

int log_buffer_set_listener(webos_log_buffer_listener_t listener, void* context) {
  int ret = 0;

  if (listener == NULL) {
    return -EINVAL;
  }
  k_mutex_lock(&log_lock, K_FOREVER);
  if (log_listener != NULL) {
    ret = -EBUSY;
  } else {
    log_listener = listener;
    log_listener_context = context;
  }
  k_mutex_unlock(&log_lock);
  return ret;
}

int log_buffer_snapshot_and_set_listener(webos_log_buffer_listener_t listener, void* context, char* dst, size_t dst_len,
                                         size_t* snapshot_len) {
  int ret = 0;

  if (listener == NULL || dst == NULL || dst_len == 0U || snapshot_len == NULL) {
    return -EINVAL;
  }
  k_mutex_lock(&log_lock, K_FOREVER);
  if (log_listener != NULL) {
    ret = -EBUSY;
  } else {
    *snapshot_len = copy_log_locked(dst, dst_len);
    log_listener = listener;
    log_listener_context = context;
  }
  k_mutex_unlock(&log_lock);
  return ret;
}

void log_buffer_clear_listener(webos_log_buffer_listener_t listener, void* context) {
  k_mutex_lock(&log_lock, K_FOREVER);
  if (log_listener == listener && log_listener_context == context) {
    log_listener = NULL;
    log_listener_context = NULL;
  }
  k_mutex_unlock(&log_lock);
}

static int char_out(uint8_t* data, size_t length, void* ctx) {
  ARG_UNUSED(ctx);

  log_buffer_put((const char*)data, length);
  return length;
}

LOG_OUTPUT_DEFINE(log_output_webos, char_out, output_buf, sizeof(output_buf));

static void process(const struct log_backend* const backend, union log_msg_generic* msg) {
  ARG_UNUSED(backend);

  uint32_t flags = log_backend_std_get_flags();
  log_format_func_t log_output_func = log_format_func_t_get(log_format_current);

  log_output_func(&log_output_webos, &msg->log, flags);
}

static int format_set(const struct log_backend* const backend, uint32_t log_type) {
  ARG_UNUSED(backend);

  log_format_current = log_type;
  return 0;
}

const struct log_backend_api log_backend_webos_api = {
    .process = process,
    .format_set = format_set,
};

LOG_BACKEND_DEFINE(log_backend_webos, log_backend_webos_api, true);
