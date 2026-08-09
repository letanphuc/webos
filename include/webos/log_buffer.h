/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WEBOS_INCLUDE_LOG_BUFFER_H_
#define WEBOS_INCLUDE_LOG_BUFFER_H_

#include <stddef.h>

typedef void (*webos_log_buffer_listener_t)(const char* data, size_t len, void* context);

void log_buffer_init(void);
void log_buffer_put(const char* msg, size_t len);
size_t log_buffer_read(char* dst, size_t dst_len);
void log_buffer_clear(void);

/* Only one live stream listener is supported. */
int log_buffer_set_listener(webos_log_buffer_listener_t listener, void* context);
int log_buffer_snapshot_and_set_listener(webos_log_buffer_listener_t listener, void* context, char* dst, size_t dst_len,
                                         size_t* snapshot_len);
void log_buffer_clear_listener(webos_log_buffer_listener_t listener, void* context);

#endif /* WEBOS_INCLUDE_LOG_BUFFER_H_ */
