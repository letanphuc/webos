#ifndef WEBOS_H_
#define WEBOS_H_

#ifdef __cplusplus
extern "C" {
#endif

void sleep_ms(unsigned int ms);
void log_print(const char* message);
int dev_fs_write(const char* path, const void* data, unsigned int length);
int dev_fs_read(const char* path, void* data, unsigned int capacity);

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
  unsigned int struct_size;
  unsigned int status_code;
  unsigned int body_len;
  unsigned int content_length;
  unsigned int flags;
};

int web_http_request(unsigned int method, const char* url, unsigned int url_len, const void* headers,
                     unsigned int headers_len, const void* request_body, unsigned int request_body_len,
                     void* response_body, unsigned int response_capacity, struct web_http_response* response,
                     unsigned int response_size, unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WEBOS_H_ */
