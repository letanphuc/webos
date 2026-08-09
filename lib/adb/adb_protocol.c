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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "adb_internal.h"
#include "adb_service.h"

LOG_MODULE_DECLARE(webos_adb, LOG_LEVEL_INF);

#define ADB_COMMAND_CNXN 0x4e584e43U
#define ADB_COMMAND_OPEN 0x4e45504fU
#define ADB_COMMAND_OKAY 0x59414b4fU
#define ADB_COMMAND_CLSE 0x45534c43U
#define ADB_COMMAND_WRTE 0x45545257U
#define ADB_VERSION 0x01000000U
#define ADB_VERSION_NO_CHECKSUM 0x01000001U

struct adb_message {
  uint32_t command;
  uint32_t arg0;
  uint32_t arg1;
  uint32_t data_length;
  uint32_t data_check;
  uint32_t magic;
};

struct webos_adb_stream {
  bool used;
  bool tx_pending;
  bool awaiting_ack;
  bool close_pending;
  uint32_t local_id;
  uint32_t remote_id;
  const struct webos_adb_service_ops* ops;
  size_t tx_len;
  uint8_t tx_data[CONFIG_WEBOS_ADB_TX_PAYLOAD_SIZE];
};

struct adb_rx_state {
  uint8_t header[WEBOS_ADB_HEADER_SIZE];
  uint8_t* payload;
  uint32_t payload_checksum;
  size_t header_used;
  size_t payload_used;
  struct adb_message message;
};

BUILD_ASSERT(sizeof(struct adb_message) == WEBOS_ADB_HEADER_SIZE);

static struct adb_rx_state rx;
static struct webos_adb_stream streams[CONFIG_WEBOS_ADB_MAX_STREAMS];
static webos_adb_send_fn transport_send;
static uint32_t next_local_id = 1U;
static size_t negotiated_payload = CONFIG_WEBOS_ADB_MAX_PAYLOAD;
static bool online;

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

static void reset_rx(void) {
  k_free(rx.payload);
  memset(&rx, 0, sizeof(rx));
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

static int send_packet(uint32_t command, uint32_t arg0, uint32_t arg1, const uint8_t* payload, size_t payload_len) {
  uint8_t header[WEBOS_ADB_HEADER_SIZE];

  if (transport_send == NULL) {
    return -ENODEV;
  }
  encode_message(header, command, arg0, arg1, payload, payload_len);
  return transport_send(header, sizeof(header), payload, payload_len);
}

static int send_connect(void) {
  return send_packet(ADB_COMMAND_CNXN, ADB_VERSION, CONFIG_WEBOS_ADB_MAX_PAYLOAD, (const uint8_t*)device_banner,
                     strlen(device_banner));
}

static int send_ready(uint32_t local_id, uint32_t remote_id) {
  return send_packet(ADB_COMMAND_OKAY, local_id, remote_id, NULL, 0U);
}

static int send_close(uint32_t local_id, uint32_t remote_id) {
  return send_packet(ADB_COMMAND_CLSE, local_id, remote_id, NULL, 0U);
}

static uint32_t allocate_local_id(void) {
  uint32_t id = next_local_id++;

  if (next_local_id == 0U) {
    next_local_id = 1U;
  }
  return id;
}

static struct webos_adb_stream* find_stream(uint32_t local_id, uint32_t remote_id) {
  for (size_t i = 0; i < ARRAY_SIZE(streams); ++i) {
    if (streams[i].used && streams[i].local_id == local_id && streams[i].remote_id == remote_id) {
      return &streams[i];
    }
  }
  return NULL;
}

static struct webos_adb_stream* allocate_stream(uint32_t remote_id, const struct webos_adb_service_ops* ops) {
  for (size_t i = 0; i < ARRAY_SIZE(streams); ++i) {
    if (!streams[i].used) {
      memset(&streams[i], 0, sizeof(streams[i]));
      streams[i].used = true;
      streams[i].local_id = allocate_local_id();
      streams[i].remote_id = remote_id;
      streams[i].ops = ops;
      return &streams[i];
    }
  }
  return NULL;
}

static void release_stream(struct webos_adb_stream* stream) {
  const struct webos_adb_service_ops* ops;

  if (stream == NULL || !stream->used) {
    return;
  }
  ops = stream->ops;
  if (ops != NULL && ops->close != NULL) {
    ops->close(stream);
  }
  memset(stream, 0, sizeof(*stream));
}

static void close_all_streams(void) {
  for (size_t i = 0; i < ARRAY_SIZE(streams); ++i) {
    release_stream(&streams[i]);
  }
}

static int flush_stream(struct webos_adb_stream* stream) {
  int ret;

  if (stream->tx_pending && !stream->awaiting_ack) {
    ret = send_packet(ADB_COMMAND_WRTE, stream->local_id, stream->remote_id, stream->tx_data, stream->tx_len);
    if (ret != 0) {
      return ret;
    }
    stream->tx_pending = false;
    stream->awaiting_ack = true;
  }

  if (stream->close_pending && !stream->tx_pending && !stream->awaiting_ack) {
    ret = send_close(stream->local_id, stream->remote_id);
    if (ret != 0) {
      return ret;
    }
    release_stream(stream);
  }
  return 0;
}

int webos_adb_stream_write(struct webos_adb_stream* stream, const uint8_t* data, size_t len) {
  if (stream == NULL || !stream->used || (data == NULL && len != 0U)) {
    return -EINVAL;
  }
  if (len > webos_adb_stream_max_payload(stream)) {
    return -EMSGSIZE;
  }
  if (stream->tx_pending || stream->awaiting_ack) {
    return -EBUSY;
  }

  if (len != 0U) {
    memcpy(stream->tx_data, data, len);
  }
  stream->tx_len = len;
  stream->tx_pending = true;
  return 0;
}

void webos_adb_stream_close(struct webos_adb_stream* stream) {
  if (stream != NULL && stream->used) {
    stream->close_pending = true;
  }
}

size_t webos_adb_stream_max_payload(const struct webos_adb_stream* stream) {
  ARG_UNUSED(stream);
  return MIN(negotiated_payload, (size_t)CONFIG_WEBOS_ADB_TX_PAYLOAD_SIZE);
}

static int validate_message(const struct adb_message* message) {
  if (message->magic != (message->command ^ UINT32_MAX)) {
    return -EBADMSG;
  }
  if (message->data_length > CONFIG_WEBOS_ADB_MAX_PAYLOAD) {
    return -EMSGSIZE;
  }
  return 0;
}

static int open_stream(const uint8_t* destination, size_t destination_len, uint32_t remote_id, const char* prefix,
                       const struct webos_adb_service_ops* ops) {
  struct webos_adb_stream* stream;
  size_t prefix_len = strlen(prefix);
  int ret;

  if (destination_len < prefix_len || memcmp(destination, prefix, prefix_len) != 0) {
    return -ENOENT;
  }

  stream = allocate_stream(remote_id, ops);
  if (stream == NULL) {
    return send_close(0U, remote_id);
  }
  ret = ops->open(stream, destination + prefix_len, destination_len - prefix_len);
  if (ret != 0) {
    release_stream(stream);
    return send_close(0U, remote_id);
  }

  ret = send_ready(stream->local_id, stream->remote_id);
  if (ret == 0) {
    ret = flush_stream(stream);
  }
  if (ret != 0) {
    release_stream(stream);
  }
  return ret;
}

static int handle_open(void) {
  const uint8_t* destination = rx.payload;
  size_t destination_len;
  int ret = -ENOENT;

  if (!online || rx.message.arg0 == 0U || rx.message.arg1 != 0U || rx.message.data_length == 0U ||
      destination == NULL || destination[rx.message.data_length - 1U] != '\0') {
    return -EBADMSG;
  }
  destination_len = rx.message.data_length - 1U;

  if ((destination_len == strlen("track-jdwp") && memcmp(destination, "track-jdwp", destination_len) == 0) ||
      (destination_len == strlen("track-app") && memcmp(destination, "track-app", destination_len) == 0)) {
    return send_ready(allocate_local_id(), rx.message.arg0);
  }

#if defined(CONFIG_WEBOS_ADB_REBOOT)
  if (destination_len >= strlen("reboot:") && memcmp(destination, "reboot:", strlen("reboot:")) == 0) {
    if (destination_len != strlen("reboot:")) {
      return send_close(0U, rx.message.arg0);
    }
    ret = send_ready(allocate_local_id(), rx.message.arg0);
    if (ret == 0) {
      ret = webos_adb_reboot_request(NULL, 0U);
    }
    return ret;
  }
#endif

#if defined(CONFIG_WEBOS_ADB_SHELL)
  ret = open_stream(destination, destination_len, rx.message.arg0, "shell:", &webos_adb_shell_service_ops);
  if (ret != -ENOENT) {
    return ret;
  }
#endif
#if defined(CONFIG_WEBOS_ADB_SYNC)
  ret = open_stream(destination, destination_len, rx.message.arg0, "sync:", &webos_adb_sync_service_ops);
  if (ret != -ENOENT) {
    return ret;
  }
#endif

  LOG_DBG("Rejecting unsupported ADB service");
  return send_close(0U, rx.message.arg0);
}

static int handle_write(void) {
  struct webos_adb_stream* stream = find_stream(rx.message.arg1, rx.message.arg0);
  int ret;

  if (stream == NULL || stream->ops == NULL || stream->ops->write == NULL) {
    return send_close(0U, rx.message.arg0);
  }

  ret = stream->ops->write(stream, rx.payload, rx.message.data_length);
  if (ret != 0) {
    stream->close_pending = true;
    stream->tx_pending = false;
    stream->awaiting_ack = false;
    return flush_stream(stream);
  }

  ret = send_ready(stream->local_id, stream->remote_id);
  if (ret == 0) {
    ret = flush_stream(stream);
  }
  return ret;
}

static int handle_ready(void) {
  struct webos_adb_stream* stream = find_stream(rx.message.arg1, rx.message.arg0);

  if (stream == NULL || !stream->awaiting_ack) {
    return 0;
  }
  stream->awaiting_ack = false;
  if (stream->ops != NULL && stream->ops->tx_ready != NULL) {
    stream->ops->tx_ready(stream);
  }
  return stream->used ? flush_stream(stream) : 0;
}

static int handle_close(void) {
  struct webos_adb_stream* stream = find_stream(rx.message.arg1, rx.message.arg0);
  int ret = 0;

  if (stream != NULL) {
    ret = send_close(stream->local_id, stream->remote_id);
    release_stream(stream);
  }
  return ret;
}

static int dispatch_message(void) {
  int ret;

  if (rx.message.data_check != rx.payload_checksum &&
      !(rx.message.command == ADB_COMMAND_CNXN && rx.message.arg0 >= ADB_VERSION_NO_CHECKSUM &&
        rx.message.data_check == 0U)) {
    return -EBADMSG;
  }

  switch (rx.message.command) {
    case ADB_COMMAND_CNXN:
#if !defined(CONFIG_WEBOS_ADB_ALLOW_NO_AUTH)
      return -EACCES;
#else
      close_all_streams();
      negotiated_payload = MIN((size_t)rx.message.arg1, (size_t)CONFIG_WEBOS_ADB_MAX_PAYLOAD);
      ret = send_connect();
      if (ret == 0) {
        online = true;
      }
      return ret;
#endif
    case ADB_COMMAND_OPEN:
      return handle_open();
    case ADB_COMMAND_WRTE:
      return handle_write();
    case ADB_COMMAND_OKAY:
      return handle_ready();
    case ADB_COMMAND_CLSE:
      return handle_close();
    default:
      return -ENOTSUP;
  }
}

static int decode_header(void) {
  int ret;

  rx.message.command = sys_get_le32(rx.header + 0);
  rx.message.arg0 = sys_get_le32(rx.header + 4);
  rx.message.arg1 = sys_get_le32(rx.header + 8);
  rx.message.data_length = sys_get_le32(rx.header + 12);
  rx.message.data_check = sys_get_le32(rx.header + 16);
  rx.message.magic = sys_get_le32(rx.header + 20);

  ret = validate_message(&rx.message);
  if (ret != 0 || rx.message.data_length == 0U) {
    return ret;
  }
  rx.payload = k_malloc(rx.message.data_length);
  return rx.payload == NULL ? -ENOMEM : 0;
}

void webos_adb_protocol_init(webos_adb_send_fn send_fn) {
  transport_send = send_fn;
  webos_adb_protocol_reset();
}

void webos_adb_protocol_reset(void) {
  reset_rx();
  close_all_streams();
  next_local_id = 1U;
  negotiated_payload = CONFIG_WEBOS_ADB_MAX_PAYLOAD;
  online = false;
}

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
        reset_rx();
        return ret;
      }
      if (rx.message.data_length == 0U) {
        ret = dispatch_message();
        reset_rx();
        if (ret != 0) {
          return ret;
        }
      }
    }

    if (rx.header_used == sizeof(rx.header) && rx.message.data_length > 0U) {
      size_t count = MIN(len, rx.message.data_length - rx.payload_used);

      memcpy(rx.payload + rx.payload_used, data, count);
      rx.payload_checksum += adb_checksum(data, count);
      rx.payload_used += count;
      data += count;
      len -= count;
      if (rx.payload_used == rx.message.data_length) {
        ret = dispatch_message();
        reset_rx();
        if (ret != 0) {
          return ret;
        }
      }
    }
  }
  return ret;
}
