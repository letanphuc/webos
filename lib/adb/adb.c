/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <webos/adb.h>
#include <zephyr/logging/log.h>

#include "adb_internal.h"

LOG_MODULE_REGISTER(webos_adb, LOG_LEVEL_INF);

#if defined(CONFIG_WEBOS_ADB_USB)
int webos_adb_usb_send(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len);
#endif

int webos_adb_init(void) {
#if defined(CONFIG_WEBOS_ADB_USB)
  int ret;

  webos_adb_protocol_init(webos_adb_usb_send);
  ret = webos_adb_usb_init();
  if (ret != 0) {
    LOG_ERR("Failed to initialize ADB USB transport: %d", ret);
    return ret;
  }

#if defined(CONFIG_WEBOS_ADB_ALLOW_NO_AUTH)
  LOG_WRN("ADB authentication is disabled; development use only");
#endif

  LOG_INF("ADB USB transport enabled");
  return 0;
#else
  return -ENOTSUP;
#endif
}
