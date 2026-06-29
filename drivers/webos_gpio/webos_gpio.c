#include "webos_gpio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "devfs.h"

LOG_MODULE_REGISTER(webos_gpio, CONFIG_FS_LOG_LEVEL);

#define DT_DRV_COMPAT webos_gpio

enum webos_gpio_file_type {
  WEBOS_GPIO_VALUE,
  WEBOS_GPIO_DIRECTION,
};

struct webos_gpio_dev;

struct webos_gpio_file_def {
  struct webos_gpio_dev* gpio;
  enum webos_gpio_file_type type;
  char path[CONFIG_WEBOS_DEVFS_MAX_PATH_LEN];
};

struct webos_gpio_dev {
  struct gpio_dt_spec spec;
  const char* name;
  struct webos_gpio_file_def value;
  struct webos_gpio_file_def direction;
  int value_state;
  bool value_set;
  bool dir_out;
  bool dir_set;
};

struct webos_gpio_open_file {
  struct webos_gpio_file_def* def;
  bool read_eof;
};

static int webos_gpio_open(struct devfs_file* file, void* user_data, int flags) {
  struct webos_gpio_file_def* def = user_data;
  struct webos_gpio_open_file* open_file;

  if (!gpio_is_ready_dt(&def->gpio->spec)) {
    return -ENODEV;
  }

  open_file = k_malloc(sizeof(*open_file));
  if (open_file == NULL) {
    return -ENOMEM;
  }

  open_file->def = def;
  open_file->read_eof = false;
  devfs_file_set_data(file, open_file);
  return 0;
}

static ssize_t webos_gpio_read(struct devfs_file* file, void* dest, size_t nbytes) {
  struct webos_gpio_open_file* open_file = devfs_file_data(file);
  struct webos_gpio_file_def* def = open_file->def;
  struct webos_gpio_dev* gpio = def->gpio;
  char buf[16];
  int len;

  if (nbytes == 0) {
    return -EIO;
  }
  if (open_file->read_eof) {
    return 0;
  }
  open_file->read_eof = true;

  if (def->type == WEBOS_GPIO_VALUE) {
    if (gpio->value_set) {
      len = snprintf(buf, sizeof(buf), "%d\n", gpio->value_state);
    } else {
      len = snprintf(buf, sizeof(buf), "?\n");
    }
  } else {
    if (gpio->dir_set) {
      len = snprintf(buf, sizeof(buf), "%s\n", gpio->dir_out ? "out" : "in");
    } else {
      len = snprintf(buf, sizeof(buf), "?\n");
    }
  }

  if ((size_t)len > nbytes) {
    len = (int)nbytes;
  }
  memcpy(dest, buf, len);
  return len;
}

static ssize_t webos_gpio_write(struct devfs_file* file, const void* src, size_t nbytes) {
  struct webos_gpio_open_file* open_file = devfs_file_data(file);
  struct webos_gpio_file_def* def = open_file->def;
  struct webos_gpio_dev* gpio = def->gpio;
  char buf[16];
  size_t copy_len;
  int ret;

  if (nbytes == 0) {
    return -EIO;
  }

  copy_len = (nbytes < sizeof(buf) - 1) ? nbytes : sizeof(buf) - 1;
  memcpy(buf, src, copy_len);
  buf[copy_len] = '\0';

  if (def->type == WEBOS_GPIO_VALUE) {
    int target;

    if (buf[0] == '1' || buf[0] == 1 || buf[0] == 'o') {
      target = 1;
    } else if (buf[0] == '0' || buf[0] == 0) {
      target = 0;
    } else {
      return -EINVAL;
    }

    ret = gpio_pin_configure_dt(&gpio->spec, GPIO_OUTPUT);
    if (ret < 0) {
      return ret;
    }
    ret = gpio_pin_set_dt(&gpio->spec, target);
    if (ret < 0) {
      return ret;
    }
    gpio->value_state = target;
    gpio->value_set = true;
    return (ssize_t)nbytes;
  }

  if (strncmp(buf, "out", 3) == 0 || buf[0] == 1) {
    ret = gpio_pin_configure_dt(&gpio->spec, GPIO_OUTPUT);
    gpio->dir_out = true;
  } else if (strncmp(buf, "in", 2) == 0 || buf[0] == 0) {
    ret = gpio_pin_configure_dt(&gpio->spec, GPIO_INPUT);
    gpio->dir_out = false;
  } else {
    return -EINVAL;
  }
  if (ret < 0) {
    return ret;
  }
  gpio->dir_set = true;
  return (ssize_t)nbytes;
}

static int webos_gpio_close(struct devfs_file* file) {
  k_free(devfs_file_data(file));
  devfs_file_set_data(file, NULL);
  return 0;
}

static const struct devfs_file_ops webos_gpio_ops = {
    .open = webos_gpio_open,
    .read = webos_gpio_read,
    .write = webos_gpio_write,
    .close = webos_gpio_close,
};

#define WEBOS_GPIO_INIT(inst)                       \
  {                                                 \
      .spec = GPIO_DT_SPEC_INST_GET(inst, gpios),   \
      .name = DT_NODE_FULL_NAME(DT_DRV_INST(inst)), \
  }

static struct webos_gpio_dev webos_gpios[] = {DT_INST_FOREACH_STATUS_OKAY(WEBOS_GPIO_INIT)};

int webos_gpio_register_devfs(void) {
  int ret;

  for (size_t i = 0; i < ARRAY_SIZE(webos_gpios); i++) {
    struct webos_gpio_dev* gpio = &webos_gpios[i];

    gpio->value.gpio = gpio;
    gpio->value.type = WEBOS_GPIO_VALUE;
    gpio->direction.gpio = gpio;
    gpio->direction.type = WEBOS_GPIO_DIRECTION;

    snprintk(gpio->value.path, sizeof(gpio->value.path), "/gpio/%u/value", gpio->spec.pin);
    snprintk(gpio->direction.path, sizeof(gpio->direction.path), "/gpio/%u/direction", gpio->spec.pin);

    ret = devfs_register_file(gpio->value.path, &webos_gpio_ops, &gpio->value);
    if (ret != 0) {
      return ret;
    }
    ret = devfs_register_file(gpio->direction.path, &webos_gpio_ops, &gpio->direction);
    if (ret != 0) {
      return ret;
    }
    LOG_INF("registered /dev/gpio/%u for %s", gpio->spec.pin, gpio->name);
  }

  return 0;
}
