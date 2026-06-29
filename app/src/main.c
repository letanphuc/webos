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
  LOG_INF("WebOS starting on ESP32-S3");

  ota_init();
  boot_write_img_confirmed();
  init_filesystem_layout();
  connect_wifi();
#if defined(CONFIG_WEBOS_DEVFS)
  devfs_register();
#endif
#if defined(CONFIG_WEBOS_GPIO)
  webos_gpio_register_devfs();
#endif
#if defined(CONFIG_WEBOS_RGBLED)
  webos_rgbled_register_devfs();
#endif
#if defined(CONFIG_WEBOS_SSH)
  ssh_service_start();
#endif
  iwasm_init();
  webos_http_init();

  return 0;
}
