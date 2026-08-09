/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "adb_internal.h"

LOG_MODULE_DECLARE(webos_adb, LOG_LEVEL_INF);

static void reboot_work_handler(struct k_work* work) {
  ARG_UNUSED(work);
  LOG_INF("Rebooting after ADB request");
  sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

int webos_adb_reboot_request(const char* target, size_t target_len) {
  int ret;

  if (target == NULL && target_len != 0U) {
    return -EINVAL;
  }

  /* WebOS currently supports only the normal cold-reboot target. */
  if (target_len != 0U) {
    return -ENOTSUP;
  }

  ret = k_work_reschedule(&reboot_work, K_MSEC(CONFIG_WEBOS_ADB_REBOOT_DELAY_MS));
  if (ret < 0) {
    return ret;
  }

  LOG_INF("ADB reboot accepted; rebooting in %d ms", CONFIG_WEBOS_ADB_REBOOT_DELAY_MS);
  return 0;
}
