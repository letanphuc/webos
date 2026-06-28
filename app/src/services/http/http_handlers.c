#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/util.h>

#include "services/http/http.h"
#include "services/http/http_handlers.h"
#include "services/fs/fs.h"
#include "services/ota/ota.h"
#include "utils/json/json.h"

LOG_MODULE_REGISTER(webos_http_handlers, LOG_LEVEL_INF);

static K_MUTEX_DEFINE(shell_lock);

static int root_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);
	static char body[96];

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		int ret = snprintk(body, sizeof(body), "Hello from WebOS %s\n", APP_VERSION_STRING);

		set_text_response(response_ctx, HTTP_200_OK, body, ret);
	}

	return 0;
}

static int health_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		static const char body[] = "{\"status\":\"ok\"}\n";
		set_json_response(response_ctx, HTTP_200_OK, body, strlen(body));
	}

	return 0;
}

static int push_handler(struct http_client_ctx *client, enum http_transaction_status status,
			 const struct http_request_ctx *request_ctx,
			 struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);
	static char body[WEBOS_HTTP_BODY_MAX];
	static size_t cursor;
	static char response[96];
	char path[128];
	char file[WEBOS_JSON_VALUE_MAX];
	int ret;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED || status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		cursor = 0;
		return 0;
	}

	if (cursor + request_ctx->data_len >= sizeof(body)) {
		cursor = 0;
		static const char too_large[] = "{\"error\":\"request too large\"}\n";
		set_json_response(response_ctx, HTTP_413_PAYLOAD_TOO_LARGE, too_large,
				  strlen(too_large));
		return 0;
	}

	memcpy(body + cursor, request_ctx->data, request_ctx->data_len);
	cursor += request_ctx->data_len;

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	body[cursor] = '\0';
	cursor = 0;

	ret = json_get_string(body, "path", path, sizeof(path));
	if (ret == 0) {
		ret = json_get_string(body, "file", file, sizeof(file));
	}
	if (ret == 0) {
		ret = write_file(path, file);
	}

	if (ret == 0) {
		ret = snprintk(response, sizeof(response), "{\"ok\":true,\"path\":\"%s\"}\n", path);
		set_json_response(response_ctx, HTTP_200_OK, response, ret);
	} else {
		ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
		set_json_response(response_ctx, HTTP_400_BAD_REQUEST, response, ret);
	}

	return 0;
}

static int shell_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);
	static char body[WEBOS_HTTP_BODY_MAX];
	static size_t cursor;
	static char cmd[256];
	static char response[CONFIG_SHELL_BACKEND_DUMMY_BUF_SIZE + 64];
	static char escaped[CONFIG_SHELL_BACKEND_DUMMY_BUF_SIZE * 2];
	const struct shell *sh;
	const char *output;
	size_t output_len;
	int ret;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED || status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		cursor = 0;
		return 0;
	}

	if (cursor + request_ctx->data_len >= sizeof(body)) {
		cursor = 0;
		static const char too_large[] = "{\"error\":\"request too large\"}\n";
		set_json_response(response_ctx, HTTP_413_PAYLOAD_TOO_LARGE, too_large,
				  strlen(too_large));
		return 0;
	}

	memcpy(body + cursor, request_ctx->data, request_ctx->data_len);
	cursor += request_ctx->data_len;

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	body[cursor] = '\0';
	cursor = 0;

	ret = json_get_string(body, "cmd", cmd, sizeof(cmd));
	if (ret != 0) {
		ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
		set_json_response(response_ctx, HTTP_400_BAD_REQUEST, response, ret);
		return 0;
	}

	k_mutex_lock(&shell_lock, K_FOREVER);
	sh = shell_backend_dummy_get_ptr();
	shell_backend_dummy_clear_output(sh);
	ret = shell_execute_cmd(sh, cmd);
	output = shell_backend_dummy_get_output(sh, &output_len);
	append_json_string(escaped, sizeof(escaped), output, output_len);
	ret = snprintk(response, sizeof(response), "{\"rc\":%d,\"output\":\"%s\"}\n", ret,
		       escaped);
	k_mutex_unlock(&shell_lock);

	set_json_response(response_ctx, HTTP_200_OK, response, ret);
	return 0;
}

static int ota_handler(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *request_ctx,
		       struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);
	static char response[96];
	int ret = 0;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		ota_abort();
		return 0;
	}

	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}

	ret = ota_begin();
	if (ret != 0) {
		ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
		set_json_response(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR, response, ret);
		return 0;
	}

	if (request_ctx->data_len > 0) {
		ret = ota_write(request_ctx->data, request_ctx->data_len,
				status == HTTP_SERVER_REQUEST_DATA_FINAL);
	}

	if (ret == 0 && status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		size_t written = 0;

		ret = ota_finish(&written);
		if (ret == 0) {
			ret = snprintk(response, sizeof(response),
				       "{\"ok\":true,\"bytes\":%zu,\"reboot_ms\":%d}\n", written,
				       WEBOS_OTA_REBOOT_DELAY_MS);
			set_json_response(response_ctx, HTTP_200_OK, response, ret);
		} else {
			ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
			set_json_response(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR, response, ret);
		}
	} else if (ret != 0) {
		ota_abort();
		ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
		set_json_response(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR, response, ret);
	}

	return 0;
}

static struct http_resource_detail_dynamic root_resource_detail = {
	.common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		   .type = HTTP_RESOURCE_TYPE_DYNAMIC},
	.cb = root_handler,
};

static struct http_resource_detail_dynamic health_resource_detail = {
	.common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		   .type = HTTP_RESOURCE_TYPE_DYNAMIC},
	.cb = health_handler,
};

static struct http_resource_detail_dynamic push_resource_detail = {
	.common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		   .type = HTTP_RESOURCE_TYPE_DYNAMIC},
	.cb = push_handler,
};

static struct http_resource_detail_dynamic shell_resource_detail = {
	.common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		   .type = HTTP_RESOURCE_TYPE_DYNAMIC},
	.cb = shell_handler,
};

static struct http_resource_detail_dynamic ota_resource_detail = {
	.common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		   .type = HTTP_RESOURCE_TYPE_DYNAMIC},
	.cb = ota_handler,
};

HTTP_RESOURCE_DEFINE(root_resource, webos_http_service, "/", &root_resource_detail);
HTTP_RESOURCE_DEFINE(health_resource, webos_http_service, "/health", &health_resource_detail);
HTTP_RESOURCE_DEFINE(push_resource, webos_http_service, "/push", &push_resource_detail);
HTTP_RESOURCE_DEFINE(shell_resource, webos_http_service, "/shell", &shell_resource_detail);
HTTP_RESOURCE_DEFINE(ota_resource, webos_http_service, "/ota", &ota_resource_detail);
