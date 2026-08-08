#include "services/http_client/http_client.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/http/parser_url.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

LOG_MODULE_REGISTER(webos_http_client, LOG_LEVEL_INF);

#define WEB_HTTP_CA_TAG 42
#define WEB_HTTP_MAX_HOST_LEN 128
#define WEB_HTTP_MAX_URI_LEN 512
#define WEB_HTTP_MAX_HEADER_LINES 16
#define WEB_HTTP_DEFAULT_TIMEOUT_MS 10000

static const uint8_t amazon_root_ca1[] = {
#include "amazon_root_ca1.der.inc"
};

static K_MUTEX_DEFINE(request_lock);
static uint8_t http_recv_buf[2048];

struct parsed_url {
  char host[WEB_HTTP_MAX_HOST_LEN];
  char port[6];
  char uri[WEB_HTTP_MAX_URI_LEN];
  bool tls;
  bool explicit_port;
};

struct response_context {
  struct web_http_response* response;
  uint8_t* body;
  uint32_t capacity;
};

static int copy_url_field(const char* url, const struct http_parser_url* parsed, enum http_parser_url_fields field,
                          char* output, size_t capacity) {
  uint16_t offset = parsed->field_data[field].off;
  uint16_t length = parsed->field_data[field].len;

  if (length == 0 || length >= capacity) {
    return -EINVAL;
  }

  memcpy(output, url + offset, length);
  output[length] = '\0';
  return 0;
}

static int parse_url(const char* url, struct parsed_url* output) {
  struct http_parser_url parsed;
  char scheme[6];
  size_t url_len = strlen(url);
  int ret;

  if (url_len == 0 || url_len >= UINT16_MAX || strchr(url, '#') != NULL) {
    return -EINVAL;
  }

  http_parser_url_init(&parsed);
  ret = http_parser_parse_url(url, url_len, 0, &parsed);
  if (ret != 0) {
    return -EINVAL;
  }

  ret = copy_url_field(url, &parsed, UF_SCHEMA, scheme, sizeof(scheme));
  if (ret != 0) {
    return ret;
  }
  if (strcmp(scheme, "https") == 0) {
    output->tls = true;
    strcpy(output->port, "443");
  } else if (strcmp(scheme, "http") == 0) {
    output->tls = false;
    strcpy(output->port, "80");
  } else {
    return -EPROTONOSUPPORT;
  }

  if ((parsed.field_set & (1U << UF_USERINFO)) != 0) {
    return -EINVAL;
  }

  ret = copy_url_field(url, &parsed, UF_HOST, output->host, sizeof(output->host));
  if (ret != 0 || strchr(output->host, ':') != NULL) {
    return -EINVAL;
  }

  if ((parsed.field_set & (1U << UF_PORT)) != 0) {
    uint16_t port = parsed.port;

    output->explicit_port = true;

    if (port == 0) {
      return -EINVAL;
    }
    snprintf(output->port, sizeof(output->port), "%u", port);
  }

  if ((parsed.field_set & (1U << UF_PATH)) != 0) {
    uint16_t path_offset = parsed.field_data[UF_PATH].off;
    size_t uri_len = url_len - path_offset;

    if (uri_len >= sizeof(output->uri)) {
      return -ENAMETOOLONG;
    }
    memcpy(output->uri, url + path_offset, uri_len);
    output->uri[uri_len] = '\0';
  } else if ((parsed.field_set & (1U << UF_QUERY)) != 0) {
    uint16_t query_offset = parsed.field_data[UF_QUERY].off;
    size_t query_len = parsed.field_data[UF_QUERY].len;

    if (query_len + 2 >= sizeof(output->uri)) {
      return -ENAMETOOLONG;
    }
    output->uri[0] = '/';
    output->uri[1] = '?';
    memcpy(output->uri + 2, url + query_offset, query_len);
    output->uri[query_len + 2] = '\0';
  } else {
    strcpy(output->uri, "/");
  }

  return 0;
}

static bool is_forbidden_header(const char* name, size_t length) {
  static const char* const forbidden[] = {
      "host",
      "content-length",
      "connection",
      "transfer-encoding",
  };

  for (size_t i = 0; i < ARRAY_SIZE(forbidden); i++) {
    if (strlen(forbidden[i]) == length && strncasecmp(name, forbidden[i], length) == 0) {
      return true;
    }
  }
  return false;
}

static int validate_headers(const char* headers) {
  const char* line = headers;
  size_t line_count = 0;

  if (headers == NULL || headers[0] == '\0') {
    return 0;
  }

  while (*line != '\0') {
    const char* end = strstr(line, "\r\n");
    const char* colon;

    if (end == NULL || end == line || ++line_count > WEB_HTTP_MAX_HEADER_LINES) {
      return -EINVAL;
    }

    colon = memchr(line, ':', end - line);
    if (colon == NULL || colon == line || is_forbidden_header(line, colon - line)) {
      return -EINVAL;
    }

    for (const char* p = line; p < colon; p++) {
      char ch = *p;
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')) {
        return -EINVAL;
      }
    }
    for (const char* p = colon + 1; p < end; p++) {
      unsigned char ch = (unsigned char)*p;
      if ((ch < 0x20 && ch != '	') || ch == 0x7f) {
        return -EINVAL;
      }
    }

    line = end + 2;
  }

  return 0;
}

static enum http_method map_method(uint32_t method) {
  switch (method) {
    case WEB_HTTP_GET:
      return HTTP_GET;
    case WEB_HTTP_POST:
      return HTTP_POST;
    case WEB_HTTP_PUT:
      return HTTP_PUT;
    case WEB_HTTP_DELETE:
      return HTTP_DELETE;
    case WEB_HTTP_PATCH:
      return HTTP_PATCH;
    case WEB_HTTP_HEAD:
      return HTTP_HEAD;
    default:
      return HTTP_METHOD_END_VALUE;
  }
}

static int response_cb(struct http_response* rsp, enum http_final_call final_data, void* user_data) {
  struct response_context* context = user_data;
  struct web_http_response* response = context->response;
  size_t remaining;

  ARG_UNUSED(final_data);

  response->status_code = rsp->http_status_code;
  if (rsp->cl_present) {
    response->content_length = rsp->content_length > UINT32_MAX ? UINT32_MAX : (uint32_t)rsp->content_length;
    response->flags &= ~WEB_HTTP_RESPONSE_LENGTH_UNKNOWN;
  } else {
    response->content_length = UINT32_MAX;
    response->flags |= WEB_HTTP_RESPONSE_LENGTH_UNKNOWN;
  }

  remaining = context->capacity - response->body_len;
  if (rsp->body_frag_len > remaining) {
    if (remaining > 0) {
      memcpy(context->body + response->body_len, rsp->body_frag_start, remaining);
      response->body_len += remaining;
    }
    response->flags |= WEB_HTTP_RESPONSE_TRUNCATED;
    return -ENOSPC;
  }

  if (rsp->body_frag_len > 0) {
    memcpy(context->body + response->body_len, rsp->body_frag_start, rsp->body_frag_len);
    response->body_len += rsp->body_frag_len;
  }
  return 0;
}

static int32_t map_request_error(int ret, bool tls, const struct web_http_response* response) {
  if ((response->flags & WEB_HTTP_RESPONSE_TRUNCATED) != 0 || ret == -ENOSPC) {
    return WEB_HTTP_ERR_TOO_LARGE;
  }
  if (ret == -ETIMEDOUT || ret == -EAGAIN) {
    return WEB_HTTP_ERR_TIMEOUT;
  }
  if (ret == -ENETDOWN || ret == -ENETUNREACH) {
    return WEB_HTTP_ERR_NO_NETWORK;
  }
  if (tls && (ret == -ECONNABORTED || ret == -ECONNRESET || ret == -EIO)) {
    return WEB_HTTP_ERR_TLS;
  }
  return WEB_HTTP_ERR_IO;
}

int webos_http_client_init(void) {
  int ret =
      tls_credential_add(WEB_HTTP_CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, amazon_root_ca1, sizeof(amazon_root_ca1));

  if (ret != 0 && ret != -EEXIST) {
    LOG_ERR("Cannot register HTTPS CA certificate: %d", ret);
    return ret;
  }

  LOG_INF("HTTP/HTTPS client initialized");
  return 0;
}

int32_t webos_http_request(uint32_t method, const char* url, const char* headers, const uint8_t* request_body,
                           uint32_t request_body_len, uint8_t* response_body, uint32_t response_capacity,
                           struct web_http_response* response, uint32_t timeout_ms) {
  struct parsed_url parsed = {0};
  struct zsock_addrinfo hints = {0};
  struct zsock_addrinfo* addresses = NULL;
  struct zsock_addrinfo* address;
  struct http_request request = {0};
  struct response_context response_context;
  const char* header_fields[2] = {NULL, NULL};
  enum http_method http_method;
  int socket_fd = -1;
  int ret;

  if (url == NULL || response == NULL || (request_body_len > 0 && request_body == NULL) ||
      (response_capacity > 0 && response_body == NULL)) {
    return WEB_HTTP_ERR_INVALID;
  }

  http_method = map_method(method);
  if (http_method == HTTP_METHOD_END_VALUE) {
    return WEB_HTTP_ERR_UNSUPPORTED;
  }

  ret = parse_url(url, &parsed);
  if (ret == -EPROTONOSUPPORT) {
    return WEB_HTTP_ERR_UNSUPPORTED;
  }
  if (ret != 0 || validate_headers(headers) != 0) {
    return WEB_HTTP_ERR_INVALID;
  }

  if (timeout_ms == 0) {
    timeout_ms = WEB_HTTP_DEFAULT_TIMEOUT_MS;
  }

  response->status_code = 0;
  response->body_len = 0;
  response->content_length = UINT32_MAX;
  response->flags = WEB_HTTP_RESPONSE_LENGTH_UNKNOWN;

  if (k_mutex_lock(&request_lock, K_NO_WAIT) != 0) {
    return WEB_HTTP_ERR_BUSY;
  }

  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  ret = zsock_getaddrinfo(parsed.host, parsed.port, &hints, &addresses);
  if (ret != 0 || addresses == NULL) {
    LOG_ERR("DNS lookup failed for %s: %d", parsed.host, ret);
    ret = WEB_HTTP_ERR_DNS;
    goto cleanup;
  }

  for (address = addresses; address != NULL; address = address->ai_next) {
    socket_fd = zsock_socket(address->ai_family, SOCK_STREAM, parsed.tls ? IPPROTO_TLS_1_2 : IPPROTO_TCP);
    if (socket_fd < 0) {
      continue;
    }

    if (parsed.tls) {
      sec_tag_t tags[] = {WEB_HTTP_CA_TAG};
      int verify = TLS_PEER_VERIFY_REQUIRED;

      if (zsock_setsockopt(socket_fd, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags)) < 0 ||
          zsock_setsockopt(socket_fd, SOL_TLS, TLS_HOSTNAME, parsed.host, strlen(parsed.host) + 1) < 0 ||
          zsock_setsockopt(socket_fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify)) < 0) {
        LOG_ERR("Cannot configure TLS socket for %s: errno %d", parsed.host, errno);
        zsock_close(socket_fd);
        socket_fd = -1;
        continue;
      }
    }

    if (zsock_connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }

    LOG_DBG("Connection to %s:%s failed: errno %d", parsed.host, parsed.port, errno);
    zsock_close(socket_fd);
    socket_fd = -1;
  }

  if (socket_fd < 0) {
    LOG_ERR("Cannot connect to %s:%s", parsed.host, parsed.port);
    ret = parsed.tls ? WEB_HTTP_ERR_TLS : WEB_HTTP_ERR_CONNECT;
    goto cleanup;
  }

  if (headers != NULL && headers[0] != '\0') {
    header_fields[0] = headers;
    request.header_fields = header_fields;
  }

  response_context.response = response;
  response_context.body = response_body;
  response_context.capacity = response_capacity;

  request.method = http_method;
  request.url = parsed.uri;
  request.host = parsed.host;
  request.port = parsed.explicit_port ? parsed.port : NULL;
  request.protocol = "HTTP/1.1";
  request.payload = (const char*)request_body;
  request.payload_len = request_body_len;
  request.response = response_cb;
  request.recv_buf = http_recv_buf;
  request.recv_buf_len = sizeof(http_recv_buf);

  ret = http_client_req(socket_fd, &request, timeout_ms, &response_context);
  if (ret < 0) {
    LOG_ERR("HTTP request to %s failed: %d", parsed.host, ret);
    ret = map_request_error(ret, parsed.tls, response);
  } else {
    ret = WEB_HTTP_OK;
  }

cleanup:
  if (socket_fd >= 0) {
    zsock_close(socket_fd);
  }
  if (addresses != NULL) {
    zsock_freeaddrinfo(addresses);
  }
  k_mutex_unlock(&request_lock);
  return ret;
}
