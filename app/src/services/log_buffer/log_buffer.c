#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_output.h>

#include "services/log_buffer/log_buffer.h"

static char log_buf[CONFIG_WEBOS_LOG_BUFFER_SIZE];
static size_t write_pos;
static K_MUTEX_DEFINE(log_lock);

static uint8_t output_buf[CONFIG_WEBOS_LOG_BACKEND_BUF_SIZE];
static uint32_t log_format_current = CONFIG_WEBOS_LOG_BACKEND_OUTPUT_DEFAULT;

void log_buffer_init(void)
{
}

void log_buffer_put(const char *msg, size_t len)
{
	if (len == 0) {
		return;
	}

	k_mutex_lock(&log_lock, K_FOREVER);

	if (len >= CONFIG_WEBOS_LOG_BUFFER_SIZE) {
		len = CONFIG_WEBOS_LOG_BUFFER_SIZE - 1;
	}

	if (write_pos + len > CONFIG_WEBOS_LOG_BUFFER_SIZE) {
		write_pos = 0;
	}

	memcpy(log_buf + write_pos, msg, len);
	write_pos += len;

	k_mutex_unlock(&log_lock);
}

size_t log_buffer_read(char *dst, size_t dst_len)
{
	size_t data_len;

	k_mutex_lock(&log_lock, K_FOREVER);

	data_len = write_pos;
	if (data_len >= dst_len) {
		data_len = dst_len - 1;
	}

	memcpy(dst, log_buf, data_len);
	dst[data_len] = '\0';

	k_mutex_unlock(&log_lock);

	return data_len;
}

void log_buffer_clear(void)
{
	k_mutex_lock(&log_lock, K_FOREVER);
	write_pos = 0;
	k_mutex_unlock(&log_lock);
}

static int char_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);

	log_buffer_put((const char *)data, length);
	return length;
}

LOG_OUTPUT_DEFINE(log_output_webos, char_out, output_buf, sizeof(output_buf));

static void process(const struct log_backend *const backend,
		    union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	uint32_t flags = log_backend_std_get_flags();
	log_format_func_t log_output_func =
		log_format_func_t_get(log_format_current);

	log_output_func(&log_output_webos, &msg->log, flags);
}

static int format_set(const struct log_backend *const backend,
		      uint32_t log_type)
{
	ARG_UNUSED(backend);

	log_format_current = log_type;
	return 0;
}

const struct log_backend_api log_backend_webos_api = {
	.process = process,
	.format_set = format_set,
};

LOG_BACKEND_DEFINE(log_backend_webos, log_backend_webos_api, true);
