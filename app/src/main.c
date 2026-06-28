#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hal/wifi/wifi.h"
#include "services/fs/fs.h"
#include "services/ota/ota.h"
#include "services/http/http.h"
#include "services/iwasm/iwasm.h"

LOG_MODULE_REGISTER(webos, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("WebOS starting on ESP32-S3");

	ota_init();
	boot_write_img_confirmed();
	init_filesystem_layout();
	connect_wifi();
	iwasm_init();
	webos_http_init();

	return 0;
}
