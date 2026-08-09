/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WEBOS_LIB_ADB_INTERNAL_H_
#define WEBOS_LIB_ADB_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#define WEBOS_ADB_HEADER_SIZE 24U

typedef int (*webos_adb_send_fn)(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len);

void webos_adb_protocol_init(webos_adb_send_fn send_fn);
void webos_adb_protocol_reset(void);
int webos_adb_protocol_receive(const uint8_t* data, size_t len);

#if defined(CONFIG_WEBOS_ADB_USB)
int webos_adb_usb_init(void);
#endif

#endif /* WEBOS_LIB_ADB_INTERNAL_H_ */
