#include <errno.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>

#include "hal/wifi/wifi.h"
#include "services/fs/fs.h"
#include "services/http/http.h"
#include "services/iwasm/iwasm.h"
#include "services/ota/ota.h"
#if defined(CONFIG_WEBOS_DEVFS)
#include "devfs.h"
#endif
#if defined(CONFIG_WEBOS_GPIO)
#include "webos_gpio.h"
#endif
#if defined(CONFIG_WEBOS_RGBLED)
#include "webos_rgbled.h"
#endif
#if defined(CONFIG_WEBOS_SSH)
#include "services/ssh/ssh.h"
#endif

LOG_MODULE_REGISTER(webos, LOG_LEVEL_INF);

int main(void) {
  struct webos_health_status health = {0};
  int ret;

  LOG_INF("WebOS starting on ESP32-S3");

  ota_init();
  health.filesystem = init_filesystem_layout();

#if defined(CONFIG_WEBOS_DEVFS)
  health.devfs = devfs_register();
#endif
#if defined(CONFIG_WEBOS_GPIO)
  health.gpio = health.devfs == 0 ? webos_gpio_register_devfs() : -ECANCELED;
#endif
#if defined(CONFIG_WEBOS_RGBLED)
  health.led = health.devfs == 0 ? webos_rgbled_register_devfs() : -ECANCELED;
#endif

  health.iwasm = iwasm_init();

  if (health.filesystem == 0 && health.devfs == 0 && health.gpio == 0 && health.led == 0 && health.iwasm == 0) {
    ret = mcuboot_swap_type();
    if (ret == BOOT_SWAP_TYPE_REVERT) {
      ret = boot_write_img_confirmed();
      if (ret != 0) {
        LOG_ERR("MCUboot image confirmation failed: %d", ret);
      }
    } else if (ret < 0) {
      LOG_WRN("Cannot read MCUboot swap state: %d", ret);
    }
  } else {
    LOG_ERR("Local service initialization failed; leaving MCUboot test image unconfirmed");
  }

  health.wifi = connect_wifi();
#if defined(CONFIG_WEBOS_SSH)
  ssh_service_start();
#endif

  webos_health_set(&health);
  ret = webos_http_init();

  if (health.filesystem == 0 && health.devfs == 0 && health.gpio == 0 && health.led == 0 && health.iwasm == 0 &&
      health.wifi == 0 && ret == 0) {
    LOG_INF("Startup: OK");
  } else {
    LOG_ERR("Startup: FAILED filesystem=%d devfs=%d gpio=%d led=%d iwasm=%d wifi=%d http=%d", health.filesystem,
            health.devfs, health.gpio, health.led, health.iwasm, health.wifi, ret);
  }

  return ret;
}
