#ifndef WEBOS_SERVICES_HTTP_H
#define WEBOS_SERVICES_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <zephyr/net/http/server.h>

#define WEBOS_HTTP_PORT 8080
#define WEBOS_HTTP_BODY_MAX 4096
#define WEBOS_JSON_VALUE_MAX 1536

extern uint16_t webos_http_port;

void set_json_response(struct http_response_ctx* ctx, enum http_status status, const char* body, size_t body_len);
void set_text_response(struct http_response_ctx* ctx, enum http_status status, const char* body, size_t body_len);
int webos_http_init(void);

#endif
