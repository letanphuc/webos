#ifndef WEBOS_SERVICES_LOG_BUFFER_H
#define WEBOS_SERVICES_LOG_BUFFER_H

#include <stddef.h>

void log_buffer_init(void);
void log_buffer_put(const char *msg, size_t len);
size_t log_buffer_read(char *dst, size_t dst_len);
void log_buffer_clear(void);

#endif
