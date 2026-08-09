/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbd.h>

#include "../adb_internal.h"

LOG_MODULE_DECLARE(webos_adb, LOG_LEVEL_INF);

#define ADB_USB_OUT_EP 0x01U
#define ADB_USB_IN_EP 0x81U
#define ADB_USB_FS_MPS 64U
#define ADB_USB_ENABLED 0
#define ADB_USB_OUT_ENGAGED 1
#define ADB_USB_IN_ENGAGED 2
#define ADB_SERIAL_PREFIX "webos-esp32s3-"
#define ADB_DEVICE_ID_BYTES 6U
#define ADB_SERIAL_CAPACITY (sizeof(ADB_SERIAL_PREFIX) + ADB_DEVICE_ID_BYTES * 2U)

struct adb_usb_descriptors {
  struct usb_if_descriptor interface;
  struct usb_ep_descriptor out_ep;
  struct usb_ep_descriptor in_ep;
  struct usb_desc_header terminator;
};

enum adb_usb_tx_phase {
  ADB_USB_TX_IDLE,
  ADB_USB_TX_HEADER,
  ADB_USB_TX_PAYLOAD,
};

struct adb_usb_tx_packet {
  uint8_t header[WEBOS_ADB_HEADER_SIZE];
  uint8_t payload[CONFIG_WEBOS_ADB_USB_TRANSFER_SIZE];
  size_t payload_len;
};

struct adb_usb_data {
  struct adb_usb_descriptors* descriptors;
  const struct usb_desc_header** fs_descriptors;
  struct usbd_class_data* class_data;
  atomic_t state;
  enum adb_usb_tx_phase tx_phase;
  struct adb_usb_tx_packet tx_queue[CONFIG_WEBOS_ADB_USB_TX_QUEUE_DEPTH];
  size_t tx_head;
  size_t tx_tail;
  size_t tx_count;
};

static struct adb_usb_descriptors adb_descriptors = {
    .interface =
        {
            .bLength = sizeof(struct usb_if_descriptor),
            .bDescriptorType = USB_DESC_INTERFACE,
            .bInterfaceNumber = 0,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = 0xff,
            .bInterfaceSubClass = 0x42,
            .bInterfaceProtocol = 0x01,
            .iInterface = 0,
        },
    .out_ep =
        {
            .bLength = sizeof(struct usb_ep_descriptor),
            .bDescriptorType = USB_DESC_ENDPOINT,
            .bEndpointAddress = ADB_USB_OUT_EP,
            .bmAttributes = USB_EP_TYPE_BULK,
            .wMaxPacketSize = sys_cpu_to_le16(ADB_USB_FS_MPS),
            .bInterval = 0,
        },
    .in_ep =
        {
            .bLength = sizeof(struct usb_ep_descriptor),
            .bDescriptorType = USB_DESC_ENDPOINT,
            .bEndpointAddress = ADB_USB_IN_EP,
            .bmAttributes = USB_EP_TYPE_BULK,
            .wMaxPacketSize = sys_cpu_to_le16(ADB_USB_FS_MPS),
            .bInterval = 0,
        },
    .terminator =
        {
            .bLength = 0,
            .bDescriptorType = 0,
        },
};

static const struct usb_desc_header* adb_fs_descriptors[] = {
    (const struct usb_desc_header*)&adb_descriptors.interface,
    (const struct usb_desc_header*)&adb_descriptors.out_ep,
    (const struct usb_desc_header*)&adb_descriptors.in_ep,
    (const struct usb_desc_header*)&adb_descriptors.terminator,
};

static struct adb_usb_data adb_usb = {
    .descriptors = &adb_descriptors,
    .fs_descriptors = adb_fs_descriptors,
};

static uint8_t bulk_out_endpoint(void) { return adb_usb.descriptors->out_ep.bEndpointAddress; }

static uint8_t bulk_in_endpoint(void) { return adb_usb.descriptors->in_ep.bEndpointAddress; }

static int submit_out(struct usbd_class_data* class_data) {
  struct net_buf* buffer;
  int ret;

  if (!atomic_test_bit(&adb_usb.state, ADB_USB_ENABLED)) {
    return -ENODEV;
  }

  if (atomic_test_and_set_bit(&adb_usb.state, ADB_USB_OUT_ENGAGED)) {
    return -EBUSY;
  }

  buffer = usbd_ep_buf_alloc(class_data, bulk_out_endpoint(), CONFIG_WEBOS_ADB_USB_TRANSFER_SIZE);
  if (buffer == NULL) {
    atomic_clear_bit(&adb_usb.state, ADB_USB_OUT_ENGAGED);
    return -ENOMEM;
  }

  ret = usbd_ep_enqueue(class_data, buffer);
  if (ret != 0) {
    net_buf_unref(buffer);
    atomic_clear_bit(&adb_usb.state, ADB_USB_OUT_ENGAGED);
  }

  return ret;
}

static int enqueue_in(struct usbd_class_data* class_data, const uint8_t* data, size_t len) {
  struct net_buf* buffer;
  int ret;

  buffer = usbd_ep_buf_alloc(class_data, bulk_in_endpoint(), len);
  if (buffer == NULL) {
    return -ENOMEM;
  }

  net_buf_add_mem(buffer, data, len);
  ret = usbd_ep_enqueue(class_data, buffer);
  if (ret != 0) {
    net_buf_unref(buffer);
  }
  return ret;
}

static void reset_tx_queue(void) {
  adb_usb.tx_phase = ADB_USB_TX_IDLE;
  adb_usb.tx_head = 0U;
  adb_usb.tx_tail = 0U;
  adb_usb.tx_count = 0U;
  atomic_clear_bit(&adb_usb.state, ADB_USB_IN_ENGAGED);
}

static int start_tx_packet(struct usbd_class_data* class_data) {
  struct adb_usb_tx_packet* packet;
  int ret;

  if (adb_usb.tx_count == 0U || atomic_test_and_set_bit(&adb_usb.state, ADB_USB_IN_ENGAGED)) {
    return adb_usb.tx_count == 0U ? -ENOENT : -EBUSY;
  }

  packet = &adb_usb.tx_queue[adb_usb.tx_head];
  adb_usb.tx_phase = ADB_USB_TX_HEADER;
  ret = enqueue_in(class_data, packet->header, sizeof(packet->header));
  if (ret != 0) {
    adb_usb.tx_phase = ADB_USB_TX_IDLE;
    atomic_clear_bit(&adb_usb.state, ADB_USB_IN_ENGAGED);
  }
  return ret;
}

int webos_adb_usb_send(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len) {
  struct usbd_class_data* class_data = adb_usb.class_data;
  struct adb_usb_tx_packet* packet;
  bool start_now;
  int ret;

  if (class_data == NULL || !atomic_test_bit(&adb_usb.state, ADB_USB_ENABLED)) {
    return -ENODEV;
  }
  if (header == NULL || header_len != WEBOS_ADB_HEADER_SIZE || (payload == NULL && payload_len != 0U)) {
    return -EINVAL;
  }
  if (payload_len > CONFIG_WEBOS_ADB_USB_TRANSFER_SIZE) {
    return -EMSGSIZE;
  }
  if (adb_usb.tx_count == ARRAY_SIZE(adb_usb.tx_queue)) {
    return -ENOSPC;
  }

  start_now = adb_usb.tx_count == 0U;
  packet = &adb_usb.tx_queue[adb_usb.tx_tail];
  memcpy(packet->header, header, header_len);
  if (payload_len != 0U) {
    memcpy(packet->payload, payload, payload_len);
  }
  packet->payload_len = payload_len;
  adb_usb.tx_tail = (adb_usb.tx_tail + 1U) % ARRAY_SIZE(adb_usb.tx_queue);
  ++adb_usb.tx_count;

  if (!start_now) {
    return 0;
  }
  ret = start_tx_packet(class_data);
  if (ret != 0) {
    reset_tx_queue();
  }
  return ret;
}

static int adb_usb_request(struct usbd_class_data* class_data, struct net_buf* buffer, int err) {
  const struct udc_buf_info* info = udc_get_buf_info(buffer);
  uint8_t endpoint = info->ep;
  bool is_out = endpoint == bulk_out_endpoint();
  bool is_in = endpoint == bulk_in_endpoint();
  int ret = 0;

  if (is_out) {
    atomic_clear_bit(&adb_usb.state, ADB_USB_OUT_ENGAGED);
    if (err == 0 && buffer->len != 0U) {
      ret = webos_adb_protocol_receive(buffer->data, buffer->len);
      if (ret != 0) {
        LOG_DBG("ADB packet rejected: %d", ret);
        ret = 0;
      }
    }
  }

  net_buf_unref(buffer);

  if (err != 0) {
    if (is_in) {
      reset_tx_queue();
      webos_adb_protocol_reset();
    }
    if (err == -ECONNABORTED) {
      return 0;
    }
    LOG_WRN("ADB USB transfer failed ep=0x%02x: %d", endpoint, err);
    return err;
  }

  if (is_in && adb_usb.tx_count != 0U) {
    struct adb_usb_tx_packet* packet = &adb_usb.tx_queue[adb_usb.tx_head];

    if (adb_usb.tx_phase == ADB_USB_TX_HEADER && packet->payload_len != 0U) {
      adb_usb.tx_phase = ADB_USB_TX_PAYLOAD;
      ret = enqueue_in(class_data, packet->payload, packet->payload_len);
      if (ret == 0) {
        return 0;
      }
      LOG_ERR("Failed to send ADB USB payload: %d", ret);
      reset_tx_queue();
      webos_adb_protocol_reset();
      return ret;
    }

    adb_usb.tx_head = (adb_usb.tx_head + 1U) % ARRAY_SIZE(adb_usb.tx_queue);
    --adb_usb.tx_count;
    adb_usb.tx_phase = ADB_USB_TX_IDLE;
    atomic_clear_bit(&adb_usb.state, ADB_USB_IN_ENGAGED);
    if (adb_usb.tx_count != 0U) {
      ret = start_tx_packet(class_data);
      if (ret != 0) {
        LOG_ERR("Failed to continue ADB USB TX queue: %d", ret);
        reset_tx_queue();
        webos_adb_protocol_reset();
        return ret;
      }
    }
  }

  if (is_out && atomic_test_bit(&adb_usb.state, ADB_USB_ENABLED)) {
    ret = submit_out(class_data);
    if (ret != 0) {
      LOG_ERR("Failed to queue ADB USB OUT request: %d", ret);
    }
  }
  return ret;
}

static void* adb_usb_get_descriptors(struct usbd_class_data* class_data, enum usbd_speed speed) {
  ARG_UNUSED(class_data);
  ARG_UNUSED(speed);
  return adb_usb.fs_descriptors;
}

static int adb_usb_class_init(struct usbd_class_data* class_data) {
  adb_usb.class_data = class_data;
  return 0;
}

static void adb_usb_enable(struct usbd_class_data* class_data) {
  int ret;

  reset_tx_queue();
  webos_adb_protocol_reset();
  atomic_set_bit(&adb_usb.state, ADB_USB_ENABLED);
  ret = submit_out(class_data);
  if (ret != 0) {
    LOG_ERR("Failed to start ADB USB OUT endpoint: %d", ret);
  } else {
    LOG_INF("ADB USB interface configured");
  }
}

static void adb_usb_disable(struct usbd_class_data* class_data) {
  ARG_UNUSED(class_data);
  atomic_clear(&adb_usb.state);
  reset_tx_queue();
  webos_adb_protocol_reset();
  LOG_INF("ADB USB interface disabled");
}

static const struct usbd_class_api adb_usb_api = {
    .request = adb_usb_request,
    .enable = adb_usb_enable,
    .disable = adb_usb_disable,
    .init = adb_usb_class_init,
    .get_desc = adb_usb_get_descriptors,
};

USBD_DEFINE_CLASS(webos_adb_0, &adb_usb_api, &adb_usb, NULL);

USBD_DEVICE_DEFINE(webos_adb_device, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), CONFIG_WEBOS_ADB_USB_VID,
                   CONFIG_WEBOS_ADB_USB_PID);
USBD_DESC_LANG_DEFINE(webos_adb_lang);
USBD_DESC_MANUFACTURER_DEFINE(webos_adb_manufacturer, CONFIG_WEBOS_ADB_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(webos_adb_product, CONFIG_WEBOS_ADB_USB_PRODUCT);
USBD_DESC_CONFIG_DEFINE(webos_adb_configuration_string, "ADB Configuration");
USBD_CONFIGURATION_DEFINE(webos_adb_configuration, 0, 50, &webos_adb_configuration_string);

static uint8_t adb_serial[ADB_SERIAL_CAPACITY];
static struct usbd_desc_node webos_adb_serial = {
    .str =
        {
            .utype = USBD_DUT_STRING_SERIAL_NUMBER,
            .ascii7 = true,
        },
    .ptr = adb_serial,
    .bLength = 2,
    .bDescriptorType = USB_DESC_STRING,
};

static int prepare_serial_number(void) {
  static const char hex[] = "0123456789abcdef";
  uint8_t device_id[ADB_DEVICE_ID_BYTES];
  ssize_t id_len;
  size_t offset;

  id_len = hwinfo_get_device_id(device_id, sizeof(device_id));
  if (id_len <= 0) {
    return id_len == 0 ? -ENODATA : (int)id_len;
  }

  memcpy(adb_serial, ADB_SERIAL_PREFIX, sizeof(ADB_SERIAL_PREFIX) - 1U);
  offset = sizeof(ADB_SERIAL_PREFIX) - 1U;
  for (ssize_t i = 0; i < id_len; ++i) {
    adb_serial[offset++] = hex[device_id[i] >> 4];
    adb_serial[offset++] = hex[device_id[i] & 0x0f];
  }
  adb_serial[offset] = '\0';
  webos_adb_serial.bLength = (uint8_t)(2U + offset * 2U);

  LOG_INF("ADB USB serial: %s", adb_serial);
  return 0;
}

static void adb_usb_message(struct usbd_context* context, const struct usbd_msg* message) {
  ARG_UNUSED(context);

  if (message->type == USBD_MSG_RESET || message->type == USBD_MSG_VBUS_REMOVED) {
    reset_tx_queue();
    webos_adb_protocol_reset();
  }

  if (message->type == USBD_MSG_UDC_ERROR || message->type == USBD_MSG_STACK_ERROR) {
    LOG_ERR("ADB USB stack event %s: %d", usbd_msg_type_string(message->type), message->status);
  } else {
    LOG_DBG("ADB USB event: %s", usbd_msg_type_string(message->type));
  }
}

int webos_adb_usb_init(void) {
  int ret;

  ret = prepare_serial_number();
  if (ret != 0) {
    LOG_ERR("Cannot create stable ADB serial: %d", ret);
    return ret;
  }

  ret = usbd_add_descriptor(&webos_adb_device, &webos_adb_lang);
  if (ret != 0) {
    return ret;
  }
  ret = usbd_add_descriptor(&webos_adb_device, &webos_adb_manufacturer);
  if (ret != 0) {
    return ret;
  }
  ret = usbd_add_descriptor(&webos_adb_device, &webos_adb_product);
  if (ret != 0) {
    return ret;
  }
  ret = usbd_add_descriptor(&webos_adb_device, &webos_adb_serial);
  if (ret != 0) {
    return ret;
  }

  ret = usbd_add_configuration(&webos_adb_device, USBD_SPEED_FS, &webos_adb_configuration);
  if (ret != 0) {
    return ret;
  }
  ret = usbd_register_class(&webos_adb_device, "webos_adb_0", USBD_SPEED_FS, 1);
  if (ret != 0) {
    return ret;
  }

  ret = usbd_device_set_code_triple(&webos_adb_device, USBD_SPEED_FS, 0, 0, 0);
  if (ret != 0) {
    return ret;
  }
  ret = usbd_msg_register_cb(&webos_adb_device, adb_usb_message);
  if (ret != 0) {
    return ret;
  }
  ret = usbd_init(&webos_adb_device);
  if (ret != 0) {
    return ret;
  }

  return usbd_enable(&webos_adb_device);
}
