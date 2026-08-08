#ifndef WEBOS_SERVICES_HTTP_CLIENT_H
#define WEBOS_SERVICES_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

enum web_http_method {
  WEB_HTTP_GET = 0,
  WEB_HTTP_POST = 1,
  WEB_HTTP_PUT = 2,
  WEB_HTTP_DELETE = 3,
  WEB_HTTP_PATCH = 4,
  WEB_HTTP_HEAD = 5,
};

enum web_http_error {
  WEB_HTTP_OK = 0,
  WEB_HTTP_ERR_INVALID = -1,
  WEB_HTTP_ERR_NO_NETWORK = -2,
  WEB_HTTP_ERR_DNS = -3,
  WEB_HTTP_ERR_CONNECT = -4,
  WEB_HTTP_ERR_TLS = -5,
  WEB_HTTP_ERR_TIMEOUT = -6,
  WEB_HTTP_ERR_TOO_LARGE = -7,
  WEB_HTTP_ERR_IO = -8,
  WEB_HTTP_ERR_DENIED = -9,
  WEB_HTTP_ERR_BUSY = -10,
  WEB_HTTP_ERR_UNSUPPORTED = -11,
};

enum web_http_response_flags {
  WEB_HTTP_RESPONSE_TRUNCATED = 1u << 0,
  WEB_HTTP_RESPONSE_LENGTH_UNKNOWN = 1u << 1,
};

struct web_http_response {
  uint32_t struct_size;
  uint32_t status_code;
  uint32_t body_len;
  uint32_t content_length;
  uint32_t flags;
};

int webos_http_client_init(void);
int32_t webos_http_request(uint32_t method, const char* url, const char* headers, const uint8_t* request_body,
                           uint32_t request_body_len, uint8_t* response_body, uint32_t response_capacity,
                           struct web_http_response* response, uint32_t timeout_ms);

#endif
