#include <errno.h>
#include <string.h>

#include <ff.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(webos, LOG_LEVEL_INF);

#define WEBOS_HTTP_PORT 8080
#define WEBOS_MOUNT_POINT "/RAM:"
#define WEBOS_HTTP_BODY_MAX 2048
#define WEBOS_JSON_VALUE_MAX 1536
#define WEBOS_OTA_REBOOT_DELAY_MS 2000
#define WIFI_RETRY_MAX 5
#define WIFI_RETRY_DELAY_MS 3000

static uint16_t webos_http_port = WEBOS_HTTP_PORT;
static K_MUTEX_DEFINE(shell_lock);
static K_SEM_DEFINE(wifi_connected, 0, 1);
static K_SEM_DEFINE(ipv4_ready, 0, 1);
static bool wifi_connect_ok;
static K_MUTEX_DEFINE(ota_lock);
static struct flash_img_context ota_flash_ctx;
static bool ota_active;
static K_WORK_DELAYABLE_DEFINE(ota_reboot_work, NULL);
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static FATFS webos_fatfs;
static struct fs_mount_t webos_mount = {
	.type = FS_FATFS,
	.mnt_point = WEBOS_MOUNT_POINT,
	.fs_data = &webos_fatfs,
	.storage_dev = (void *)"RAM",
	.flags = FS_MOUNT_FLAG_USE_DISK_ACCESS,
};

static const struct http_header json_headers[] = {
	{.name = "Content-Type", .value = "application/json"},
};

static const struct http_header text_headers[] = {
	{.name = "Content-Type", .value = "text/plain; charset=utf-8"},
};

static void ota_reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Rebooting to apply OTA image");
	sys_reboot(SYS_REBOOT_COLD);
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = cb->info;

		if (status != NULL && status->status != 0) {
			LOG_ERR("Wi-Fi connect failed: %d", status->status);
			k_sem_give(&wifi_connected);
			return;
		}

		LOG_INF("Wi-Fi connected to %s", CONFIG_WEBOS_WIFI_SSID);
		wifi_connect_ok = true;
		k_sem_give(&wifi_connected);
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_WRN("Wi-Fi disconnected from %s", CONFIG_WEBOS_WIFI_SSID);
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		char addr[NET_IPV4_ADDR_LEN];

		for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type != NET_ADDR_DHCP) {
				continue;
			}

			LOG_INF("IPv4 address: %s",
				net_addr_ntop(NET_AF_INET,
					      &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
					      addr, sizeof(addr)));
			k_sem_give(&ipv4_ready);
			return;
		}
	}
}

static int connect_wifi(void)
{
	struct net_if *iface = net_if_get_wifi_sta();
	static struct wifi_connect_req_params sta_config;
	int ret;

	if (strlen(CONFIG_WEBOS_WIFI_SSID) == 0) {
		LOG_WRN("Wi-Fi SSID is not configured; skipping auto-connect");
		return -ENOTSUP;
	}

	if (iface == NULL) {
		iface = net_if_get_default();
	}
	if (iface == NULL) {
		LOG_ERR("No network interface available for Wi-Fi STA");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	sta_config.ssid = (const uint8_t *)CONFIG_WEBOS_WIFI_SSID;
	sta_config.ssid_length = strlen(CONFIG_WEBOS_WIFI_SSID);
	sta_config.psk = (const uint8_t *)CONFIG_WEBOS_WIFI_PSK;
	sta_config.psk_length = strlen(CONFIG_WEBOS_WIFI_PSK);
	sta_config.security = WIFI_SECURITY_TYPE_PSK;
	sta_config.channel = WIFI_CHANNEL_ANY;
	sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;

	for (int attempt = 1; attempt <= WIFI_RETRY_MAX; attempt++) {
		while (k_sem_take(&wifi_connected, K_NO_WAIT) == 0) {
		}
		wifi_connect_ok = false;

		LOG_INF("Connecting to Wi-Fi SSID: %s (attempt %d/%d)", CONFIG_WEBOS_WIFI_SSID,
			attempt, WIFI_RETRY_MAX);
		ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &sta_config,
			       sizeof(struct wifi_connect_req_params));
		if (ret != 0) {
			LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", ret);
			if (attempt < WIFI_RETRY_MAX) {
				k_sleep(K_MSEC(WIFI_RETRY_DELAY_MS));
			}
			continue;
		}

		ret = k_sem_take(&wifi_connected, K_SECONDS(30));
		if (ret == 0 && wifi_connect_ok) {
			break;
		}
		if (ret == 0) {
			ret = -ECONNREFUSED;
			LOG_WRN("Wi-Fi connect attempt %d/%d: failed", attempt, WIFI_RETRY_MAX);
		} else {
			LOG_WRN("Wi-Fi connect attempt %d/%d: timed out", attempt, WIFI_RETRY_MAX);
		}
		if (attempt < WIFI_RETRY_MAX) {
			k_sleep(K_MSEC(WIFI_RETRY_DELAY_MS));
		}
	}

	if (ret != 0) {
		LOG_ERR("Wi-Fi connection failed after %d attempts", WIFI_RETRY_MAX);
		return ret;
	}

	net_dhcpv4_start(iface);
	ret = k_sem_take(&ipv4_ready, K_SECONDS(30));
	if (ret != 0) {
		LOG_WRN("Timed out waiting for DHCP IPv4 address");
	}

	return 0;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
	char pattern[32];
	const char *p;
	size_t i = 0;

	if (snprintk(pattern, sizeof(pattern), "\"%s\"", key) >= sizeof(pattern)) {
		return -EINVAL;
	}

	p = strstr(json, pattern);
	if (p == NULL) {
		return -ENOENT;
	}

	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	if (*p++ != ':') {
		return -EINVAL;
	}
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	if (*p++ != '"') {
		return -EINVAL;
	}

	while (*p != '\0' && *p != '"') {
		if (i + 1 >= out_len) {
			return -ENOMEM;
		}
		if (*p == '\\') {
			p++;
			if (*p == '\0') {
				return -EINVAL;
			}
			switch (*p) {
			case 'n':
				out[i++] = '\n';
				break;
			case 'r':
				out[i++] = '\r';
				break;
			case 't':
				out[i++] = '\t';
				break;
			default:
				out[i++] = *p;
				break;
			}
		} else {
			out[i++] = *p;
		}
		p++;
	}

	if (*p != '"') {
		return -EINVAL;
	}
	out[i] = '\0';
	return 0;
}

static bool webos_path_allowed(const char *path)
{
	return strncmp(path, WEBOS_MOUNT_POINT "/", strlen(WEBOS_MOUNT_POINT "/")) == 0 &&
	       strstr(path, "..") == NULL;
}

static int ensure_dir(const char *path)
{
	int ret = fs_mkdir(path);

	return ret == -EEXIST ? 0 : ret;
}

static int write_file(const char *path, const char *data)
{
	struct fs_file_t file;
	int ret;

	if (!webos_path_allowed(path)) {
		return -EINVAL;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_TRUNC | FS_O_WRITE);
	if (ret != 0) {
		return ret;
	}

	ret = fs_write(&file, data, strlen(data));
	if (ret >= 0 && ret != strlen(data)) {
		ret = -EIO;
	}

	int close_ret = fs_close(&file);
	return ret < 0 ? ret : close_ret;
}

static void set_json_response(struct http_response_ctx *response_ctx, enum http_status status,
			      const char *body, size_t body_len)
{
	response_ctx->status = status;
	response_ctx->headers = json_headers;
	response_ctx->header_count = ARRAY_SIZE(json_headers);
	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = body_len;
	response_ctx->final_chunk = true;
}

static void set_text_response(struct http_response_ctx *response_ctx, enum http_status status,
			      const char *body, size_t body_len)
{
	response_ctx->status = status;
	response_ctx->headers = text_headers;
	response_ctx->header_count = ARRAY_SIZE(text_headers);
	response_ctx->body = (const uint8_t *)body;
	response_ctx->body_len = body_len;
	response_ctx->final_chunk = true;
}

static size_t append_json_string(char *dst, size_t dst_len, const char *src, size_t src_len)
{
	size_t pos = 0;

	for (size_t i = 0; i < src_len && pos + 1 < dst_len; i++) {
		switch (src[i]) {
		case '\\':
		case '"':
			if (pos + 2 >= dst_len) {
				return pos;
			}
			dst[pos++] = '\\';
			dst[pos++] = src[i];
			break;
		case '\n':
			if (pos + 2 >= dst_len) {
				return pos;
			}
			dst[pos++] = '\\';
			dst[pos++] = 'n';
			break;
		case '\r':
			if (pos + 2 >= dst_len) {
				return pos;
			}
			dst[pos++] = '\\';
			dst[pos++] = 'r';
			break;
		default:
			dst[pos++] = src[i];
			break;
		}
	}

	dst[pos] = '\0';
	return pos;
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
		k_mutex_lock(&ota_lock, K_FOREVER);
		ota_active = false;
		k_mutex_unlock(&ota_lock);
		return 0;
	}

	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}

	k_mutex_lock(&ota_lock, K_FOREVER);
	if (!ota_active) {
		ret = flash_img_init(&ota_flash_ctx);
		if (ret != 0) {
			ota_active = false;
			k_mutex_unlock(&ota_lock);
			ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
			set_json_response(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR, response, ret);
			return 0;
		}
		ota_active = true;
		LOG_INF("OTA upload started");
	}

	if (request_ctx->data_len > 0) {
		ret = flash_img_buffered_write(&ota_flash_ctx, request_ctx->data, request_ctx->data_len,
					       status == HTTP_SERVER_REQUEST_DATA_FINAL);
	}

	if (ret == 0 && status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		size_t written = flash_img_bytes_written(&ota_flash_ctx);

		ota_active = false;
		ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
		if (ret == 0) {
			LOG_INF("OTA upload complete: %zu bytes", written);
			k_work_reschedule(&ota_reboot_work, K_MSEC(WEBOS_OTA_REBOOT_DELAY_MS));
			ret = snprintk(response, sizeof(response),
				       "{\"ok\":true,\"bytes\":%zu,\"reboot_ms\":%d}\n", written,
				       WEBOS_OTA_REBOOT_DELAY_MS);
			set_json_response(response_ctx, HTTP_200_OK, response, ret);
		} else {
			ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
			set_json_response(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR, response, ret);
		}
	} else if (ret != 0) {
		ota_active = false;
		ret = snprintk(response, sizeof(response), "{\"error\":%d}\n", ret);
		set_json_response(response_ctx, HTTP_500_INTERNAL_SERVER_ERROR, response, ret);
	}
	k_mutex_unlock(&ota_lock);

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

HTTP_SERVICE_DEFINE(webos_http_service, NULL, &webos_http_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 4,
		    NULL, NULL, NULL);
HTTP_RESOURCE_DEFINE(root_resource, webos_http_service, "/", &root_resource_detail);
HTTP_RESOURCE_DEFINE(health_resource, webos_http_service, "/health", &health_resource_detail);
HTTP_RESOURCE_DEFINE(push_resource, webos_http_service, "/push", &push_resource_detail);
HTTP_RESOURCE_DEFINE(shell_resource, webos_http_service, "/shell", &shell_resource_detail);
HTTP_RESOURCE_DEFINE(ota_resource, webos_http_service, "/ota", &ota_resource_detail);

static void init_filesystem_layout(void)
{
	int ret;

	ret = fs_mount(&webos_mount);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("Filesystem mount failed: %d", ret);
		return;
	}

	ret = ensure_dir(WEBOS_MOUNT_POINT "/apps");
	if (ret != 0) {
		LOG_ERR("/apps init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/config");
	if (ret != 0) {
		LOG_ERR("/config init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/logs");
	if (ret != 0) {
		LOG_ERR("/logs init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/ota");
	if (ret != 0) {
		LOG_ERR("/ota init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/www");
	if (ret != 0) {
		LOG_ERR("/www init failed: %d", ret);
	}
}

int main(void)
{
	LOG_INF("WebOS starting on ESP32-S3");
	k_work_init_delayable(&ota_reboot_work, ota_reboot_handler);
	boot_write_img_confirmed();

	init_filesystem_layout();
	connect_wifi();

	int ret = http_server_start();
	if (ret != 0) {
		LOG_ERR("HTTP server failed to start: %d", ret);
	} else {
		LOG_INF("HTTP server listening on port %u", webos_http_port);
	}
}
