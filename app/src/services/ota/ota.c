#include <errno.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "services/ota/ota.h"

LOG_MODULE_REGISTER(webos_ota, LOG_LEVEL_INF);

static K_MUTEX_DEFINE(ota_lock);
static struct flash_img_context ota_flash_ctx;
static bool ota_active;

static void ota_reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Rebooting to apply OTA image");
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(ota_reboot_work, ota_reboot_handler);

void ota_init(void)
{
	k_work_init_delayable(&ota_reboot_work, ota_reboot_handler);
}

int ota_begin(void)
{
	int ret;

	k_mutex_lock(&ota_lock, K_FOREVER);

	if (ota_active) {
		k_mutex_unlock(&ota_lock);
		return 0;
	}

	ret = flash_img_init(&ota_flash_ctx);
	if (ret != 0) {
		k_mutex_unlock(&ota_lock);
		return ret;
	}

	ota_active = true;
	LOG_INF("OTA upload started");

	k_mutex_unlock(&ota_lock);
	return 0;
}

int ota_write(const uint8_t *data, size_t len, bool final)
{
	int ret;

	if (len == 0) {
		return 0;
	}

	k_mutex_lock(&ota_lock, K_FOREVER);

	if (!ota_active) {
		k_mutex_unlock(&ota_lock);
		return -ECANCELED;
	}

	ret = flash_img_buffered_write(&ota_flash_ctx, data, len, final);

	k_mutex_unlock(&ota_lock);
	return ret;
}

int ota_finish(size_t *written)
{
	int ret;

	k_mutex_lock(&ota_lock, K_FOREVER);

	*written = flash_img_bytes_written(&ota_flash_ctx);
	ota_active = false;

	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret == 0) {
		LOG_INF("OTA upload complete: %zu bytes", *written);
		k_work_reschedule(&ota_reboot_work, K_MSEC(WEBOS_OTA_REBOOT_DELAY_MS));
	}

	k_mutex_unlock(&ota_lock);
	return ret;
}

void ota_abort(void)
{
	k_mutex_lock(&ota_lock, K_FOREVER);
	ota_active = false;
	k_mutex_unlock(&ota_lock);
}
