/*
 * ADB protocol constants and behavior are derived from Android Open Source
 * Project system/core/adb, licensed under Apache-2.0.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "adb_internal.h"

LOG_MODULE_DECLARE(webos_adb, LOG_LEVEL_INF);

#define ADB_COMMAND_CNXN 0x4e584e43U
#define ADB_VERSION 0x01000000U
#define ADB_VERSION_SKIP_CHECKSUM 0x01000001U

struct adb_message {
  uint32_t command;
  uint32_t arg0;
  uint32_t arg1;
  uint32_t data_length;
  uint32_t data_check;
  uint32_t magic;
};

BUILD_ASSERT(sizeof(struct adb_message) == WEBOS_ADB_HEADER_SIZE);

struct adb_rx_state {
  uint8_t header[WEBOS_ADB_HEADER_SIZE];
  uint32_t payload_checksum;
  size_t header_used;
  size_t payload_used;
  struct adb_message message;
};

static struct adb_rx_state rx;
static webos_adb_send_fn transport_send;

static const char device_banner[] =
    "device::ro.product.name=webos;ro.product.model=WebOS_ESP32-S3;"
    "ro.product.device=webos_esp32s3;features=";

static uint32_t adb_checksum(const uint8_t* data, size_t len) {
  uint32_t sum = 0;

  for (size_t i = 0; i < len; ++i) {
    sum += data[i];
  }

  return sum;
}

static void encode_message(uint8_t output[WEBOS_ADB_HEADER_SIZE], uint32_t command, uint32_t arg0, uint32_t arg1,
                           const uint8_t* payload, size_t payload_len) {
  sys_put_le32(command, output + 0);
  sys_put_le32(arg0, output + 4);
  sys_put_le32(arg1, output + 8);
  sys_put_le32((uint32_t)payload_len, output + 12);
  sys_put_le32(adb_checksum(payload, payload_len), output + 16);
  sys_put_le32(command ^ UINT32_MAX, output + 20);
}

static int send_connect(void) {
  uint8_t header[WEBOS_ADB_HEADER_SIZE];

  encode_message(header, ADB_COMMAND_CNXN, ADB_VERSION, CONFIG_WEBOS_ADB_MAX_PAYLOAD, (const uint8_t*)device_banner,
                 strlen(device_banner));

  return transport_send(header, sizeof(header), (const uint8_t*)device_banner, strlen(device_banner));
}

static int validate_message(const struct adb_message* message) {
  if (message->magic != (message->command ^ UINT32_MAX)) {
    LOG_WRN("Rejecting ADB packet with invalid magic");
    return -EBADMSG;
  }

  if (message->data_length > CONFIG_WEBOS_ADB_MAX_PAYLOAD) {
    LOG_WRN("Rejecting oversized ADB payload: %u", message->data_length);
    return -EMSGSIZE;
  }

  return 0;
}

static int dispatch_message(void) {
  uint32_t checksum;

  checksum = rx.payload_checksum;
  if (rx.message.data_check != checksum &&
      !(rx.message.arg0 >= ADB_VERSION_SKIP_CHECKSUM && rx.message.data_check == 0U)) {
    LOG_WRN("Rejecting ADB packet with invalid checksum");
    return -EBADMSG;
  }

  if (rx.message.command != ADB_COMMAND_CNXN) {
    LOG_DBG("Ignoring unsupported ADB command 0x%08x", rx.message.command);
    return -ENOTSUP;
  }

#if !defined(CONFIG_WEBOS_ADB_ALLOW_NO_AUTH)
  LOG_WRN("ADB host connected, but authentication is not implemented yet");
  return -EACCES;
#else
  LOG_INF("ADB host handshake version=0x%08x max_payload=%u", rx.message.arg0, rx.message.arg1);
  return send_connect();
#endif
}

static int decode_header(void) {
  rx.message.command = sys_get_le32(rx.header + 0);
  rx.message.arg0 = sys_get_le32(rx.header + 4);
  rx.message.arg1 = sys_get_le32(rx.header + 8);
  rx.message.data_length = sys_get_le32(rx.header + 12);
  rx.message.data_check = sys_get_le32(rx.header + 16);
  rx.message.magic = sys_get_le32(rx.header + 20);

  return validate_message(&rx.message);
}

void webos_adb_protocol_init(webos_adb_send_fn send_fn) {
  transport_send = send_fn;
  webos_adb_protocol_reset();
}

void webos_adb_protocol_reset(void) { memset(&rx, 0, sizeof(rx)); }

int webos_adb_protocol_receive(const uint8_t* data, size_t len) {
  int ret = 0;

  if (data == NULL && len != 0U) {
    return -EINVAL;
  }

  while (len > 0U) {
    if (rx.header_used < sizeof(rx.header)) {
      size_t count = MIN(len, sizeof(rx.header) - rx.header_used);

      memcpy(rx.header + rx.header_used, data, count);
      rx.header_used += count;
      data += count;
      len -= count;

      if (rx.header_used != sizeof(rx.header)) {
        continue;
      }

      ret = decode_header();
      if (ret != 0) {
        webos_adb_protocol_reset();
        return ret;
      }

      if (rx.message.data_length == 0U) {
        ret = dispatch_message();
        webos_adb_protocol_reset();
        if (ret != 0) {
          return ret;
        }
      }
    }

    if (rx.header_used == sizeof(rx.header) && rx.message.data_length > 0U) {
      size_t count = MIN(len, rx.message.data_length - rx.payload_used);

      rx.payload_checksum += adb_checksum(data, count);
      rx.payload_used += count;
      data += count;
      len -= count;

      if (rx.payload_used == rx.message.data_length) {
        ret = dispatch_message();
        webos_adb_protocol_reset();
        if (ret != 0) {
          return ret;
        }
      }
    }
  }

  return ret;
}
