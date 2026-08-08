#ifndef WEBOS_SERVICES_HTTP_CLIENT_H
#define WEBOS_SERVICES_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "webos.h"

int webos_http_client_init(void);
int32_t webos_http_request(uint32_t method, const char* url, const char* headers, const uint8_t* request_body,
                           uint32_t request_body_len, uint8_t* response_body, uint32_t response_capacity,
                           struct web_http_response* response, uint32_t timeout_ms);

#endif
