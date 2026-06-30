#include "webos_rgbled.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "devfs.h"

LOG_MODULE_REGISTER(webos_rgbled, LOG_LEVEL_DBG);

#define DT_DRV_COMPAT webos_rgbled

struct webos_rgbled_dev {
  const struct device* strip;
  const char* name;
  uint32_t webos_pin;
  uint32_t chain_length;
  char path[CONFIG_WEBOS_DEVFS_MAX_PATH_LEN];
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

struct webos_rgbled_open_file {
  struct webos_rgbled_dev* led;
  bool read_eof;
};

static int webos_rgbled_show(struct webos_rgbled_dev* led) {
  struct led_rgb pixels[CONFIG_WEBOS_RGBLED_MAX_PIXELS];
  size_t count = led->chain_length;
  int ret;

  if (count > ARRAY_SIZE(pixels)) {
    count = ARRAY_SIZE(pixels);
  }

  for (size_t i = 0; i < count; i++) {
    pixels[i].r = led->red;
    pixels[i].g = led->green;
    pixels[i].b = led->blue;
  }

  LOG_DBG("show /dev/rgbled/%u/color rgb=%u,%u,%u via %s pixels=%zu", led->webos_pin, led->red, led->green, led->blue,
          led->strip->name, count);

  ret = led_strip_update_rgb(led->strip, pixels, count);
  if (ret < 0) {
    LOG_ERR("update %s failed: %d", led->strip->name, ret);
    return ret;
  }

  LOG_DBG("show complete /dev/rgbled/%u/color", led->webos_pin);
  return 0;
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -EINVAL;
}

static int parse_hex_color(const char* buf, uint8_t* red, uint8_t* green, uint8_t* blue) {
  int nibbles[6];

  if (buf[0] == '#') {
    buf++;
  }
  for (size_t i = 0; i < ARRAY_SIZE(nibbles); i++) {
    nibbles[i] = hex_nibble(buf[i]);
    if (nibbles[i] < 0) {
      return -EINVAL;
    }
  }
  if (isxdigit((unsigned char)buf[6])) {
    return -EINVAL;
  }

  *red = (uint8_t)((nibbles[0] << 4) | nibbles[1]);
  *green = (uint8_t)((nibbles[2] << 4) | nibbles[3]);
  *blue = (uint8_t)((nibbles[4] << 4) | nibbles[5]);
  return 0;
}

static int parse_rgb_color(char* buf, uint8_t* red, uint8_t* green, uint8_t* blue) {
  char* saveptr;
  char* token;
  long values[3];

  for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
    token = strtok_r(i == 0 ? buf : NULL, ", ", &saveptr);
    if (token == NULL) {
      return -EINVAL;
    }
    values[i] = strtol(token, NULL, 10);
    if (values[i] < 0 || values[i] > 255) {
      return -EINVAL;
    }
  }
  if (strtok_r(NULL, ", ", &saveptr) != NULL) {
    return -EINVAL;
  }

  *red = (uint8_t)values[0];
  *green = (uint8_t)values[1];
  *blue = (uint8_t)values[2];
  return 0;
}

static int parse_color(char* buf, uint8_t* red, uint8_t* green, uint8_t* blue) {
  size_t len = strlen(buf);

  LOG_DBG("parse color input '%s'", buf);

  while (len > 0 && isspace((unsigned char)buf[len - 1])) {
    buf[--len] = '\0';
  }
  while (isspace((unsigned char)*buf)) {
    buf++;
  }

  if (strcmp(buf, "off") == 0 || strcmp(buf, "clear") == 0 || strcmp(buf, "0") == 0) {
    *red = 0;
    *green = 0;
    *blue = 0;
    return 0;
  }
  if (strcmp(buf, "red") == 0) {
    *red = 255;
    *green = 0;
    *blue = 0;
    return 0;
  }
  if (strcmp(buf, "green") == 0) {
    *red = 0;
    *green = 255;
    *blue = 0;
    return 0;
  }
  if (strcmp(buf, "blue") == 0) {
    *red = 0;
    *green = 0;
    *blue = 255;
    return 0;
  }
  if (strcmp(buf, "white") == 0) {
    *red = 255;
    *green = 255;
    *blue = 255;
    return 0;
  }

  if (buf[0] == '#' || strlen(buf) == 6) {
    return parse_hex_color(buf, red, green, blue);
  }
  return parse_rgb_color(buf, red, green, blue);
}

static int webos_rgbled_open(struct devfs_file* file, void* user_data, int flags) {
  struct webos_rgbled_dev* led = user_data;
  struct webos_rgbled_open_file* open_file;

  ARG_UNUSED(flags);

  LOG_DBG("open /dev/rgbled/%u/color flags=0x%x", led->webos_pin, flags);

  if (!device_is_ready(led->strip)) {
    LOG_ERR("LED strip device %s is not ready", led->strip->name);
    return -ENODEV;
  }

  open_file = k_malloc(sizeof(*open_file));
  if (open_file == NULL) {
    return -ENOMEM;
  }

  open_file->led = led;
  open_file->read_eof = false;
  devfs_file_set_data(file, open_file);
  return 0;
}

static ssize_t webos_rgbled_read(struct devfs_file* file, void* dest, size_t nbytes) {
  struct webos_rgbled_open_file* open_file = devfs_file_data(file);
  struct webos_rgbled_dev* led = open_file->led;
  char buf[16];
  int len;

  if (nbytes == 0) {
    return -EIO;
  }
  if (open_file->read_eof) {
    return 0;
  }
  open_file->read_eof = true;

  len = snprintf(buf, sizeof(buf), "%u,%u,%u\n", led->red, led->green, led->blue);
  LOG_DBG("read /dev/rgbled/%u/color -> %s", led->webos_pin, buf);
  if ((size_t)len > nbytes) {
    len = (int)nbytes;
  }
  memcpy(dest, buf, len);
  return len;
}

static ssize_t webos_rgbled_write(struct devfs_file* file, const void* src, size_t nbytes) {
  struct webos_rgbled_open_file* open_file = devfs_file_data(file);
  struct webos_rgbled_dev* led = open_file->led;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  char buf[32];
  size_t copy_len;
  int ret;

  if (nbytes == 0) {
    return -EIO;
  }

  if (nbytes == 1 && *((const uint8_t*)src) == 0) {
    led->red = 0;
    led->green = 0;
    led->blue = 0;
    LOG_DBG("write /dev/rgbled/%u/color raw off", led->webos_pin);

    ret = webos_rgbled_show(led);
    if (ret < 0) {
      return ret;
    }
    return (ssize_t)nbytes;
  }

  if (nbytes == 3) {
    const uint8_t* rgb = src;

    led->red = rgb[0];
    led->green = rgb[1];
    led->blue = rgb[2];
    LOG_DBG("write /dev/rgbled/%u/color raw rgb=%u,%u,%u", led->webos_pin, led->red, led->green, led->blue);

    ret = webos_rgbled_show(led);
    if (ret < 0) {
      return ret;
    }
    return (ssize_t)nbytes;
  }

  copy_len = (nbytes < sizeof(buf) - 1) ? nbytes : sizeof(buf) - 1;
  memcpy(buf, src, copy_len);
  buf[copy_len] = '\0';

  LOG_DBG("write /dev/rgbled/%u/color bytes=%zu data='%s'", led->webos_pin, nbytes, buf);

  ret = parse_color(buf, &red, &green, &blue);
  if (ret < 0) {
    LOG_ERR("parse /dev/rgbled/%u/color failed: %d", led->webos_pin, ret);
    return ret;
  }

  LOG_DBG("parsed /dev/rgbled/%u/color rgb=%u,%u,%u", led->webos_pin, red, green, blue);

  led->red = red;
  led->green = green;
  led->blue = blue;
  ret = webos_rgbled_show(led);
  if (ret < 0) {
    return ret;
  }
  return (ssize_t)nbytes;
}

static int webos_rgbled_close(struct devfs_file* file) {
  struct webos_rgbled_open_file* open_file = devfs_file_data(file);

  if (open_file != NULL) {
    LOG_DBG("close /dev/rgbled/%u/color", open_file->led->webos_pin);
  }
  k_free(devfs_file_data(file));
  devfs_file_set_data(file, NULL);
  return 0;
}

static const struct devfs_file_ops webos_rgbled_ops = {
    .open = webos_rgbled_open,
    .read = webos_rgbled_read,
    .write = webos_rgbled_write,
    .close = webos_rgbled_close,
};

#define WEBOS_RGBLED_INIT(inst)                                          \
  {                                                                      \
      .strip = DEVICE_DT_GET(DT_INST_PHANDLE(inst, led)),                \
      .name = DT_NODE_FULL_NAME(DT_DRV_INST(inst)),                      \
      .webos_pin = DT_INST_PROP(inst, webos_pin),                        \
      .chain_length = DT_PROP(DT_INST_PHANDLE(inst, led), chain_length), \
  }

static struct webos_rgbled_dev webos_rgbleds[] = {DT_INST_FOREACH_STATUS_OKAY(WEBOS_RGBLED_INIT)};

int webos_rgbled_register_devfs(void) {
  int ret;

  for (size_t i = 0; i < ARRAY_SIZE(webos_rgbleds); i++) {
    struct webos_rgbled_dev* led = &webos_rgbleds[i];

    snprintk(led->path, sizeof(led->path), "/rgbled/%u/color", led->webos_pin);

    LOG_DBG("registering %s for %s logical pin %u chain length %u", led->path, led->strip->name, led->webos_pin,
            led->chain_length);

    ret = devfs_register_file(led->path, &webos_rgbled_ops, led);
    if (ret != 0) {
      return ret;
    }
    ret = webos_rgbled_show(led);
    if (ret != 0) {
      return ret;
    }
    LOG_INF("registered /dev/rgbled/%u for %s", led->webos_pin, led->name);
  }

  return 0;
}
