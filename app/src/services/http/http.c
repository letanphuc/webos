#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/sys/util.h>

#include "services/http/http.h"

LOG_MODULE_REGISTER(webos_http, LOG_LEVEL_INF);

uint16_t webos_http_port = WEBOS_HTTP_PORT;

static const struct http_header json_headers[] = {
	{.name = "Content-Type", .value = "application/json"},
};

static const struct http_header text_headers[] = {
	{.name = "Content-Type", .value = "text/plain; charset=utf-8"},
};

void set_json_response(struct http_response_ctx *response_ctx, enum http_status status,
		       const char *body, size_t body_len)
{
	response_ctx->status = status;
	response_ctx->headers = json_headers;
	response_ctx->header_count = ARRAY_SIZE(json_headers);
	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = body_len;
	response_ctx->final_chunk = true;
}

void set_text_response(struct http_response_ctx *response_ctx, enum http_status status,
		       const char *body, size_t body_len)
{
	response_ctx->status = status;
	response_ctx->headers = text_headers;
	response_ctx->header_count = ARRAY_SIZE(text_headers);
	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = body_len;
	response_ctx->final_chunk = true;
}

HTTP_SERVICE_DEFINE(webos_http_service, NULL, &webos_http_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 4,
		    NULL, NULL, NULL);

int webos_http_init(void)
{
	int ret = http_server_start();

	if (ret != 0) {
		LOG_ERR("HTTP server failed to start: %d", ret);
	} else {
		LOG_INF("HTTP server listening on port %u", webos_http_port);
	}

	return ret;
}
