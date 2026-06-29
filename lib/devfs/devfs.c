#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "devfs.h"

LOG_MODULE_REGISTER(devfs, CONFIG_FS_LOG_LEVEL);

/* Mount the devfs at /dev so payloads see:
 *   /dev/gpio/<pin>/value      "0" or "1"
 *   /dev/gpio/<pin>/direction  "in" or "out"
 */
#define DEVFS_MOUNT_POINT "/dev"
#define DEVFS_MAX_PIN     48
#define DEVFS_INVALID_PIN 0xFF

enum devfs_node_type {
	DEVFS_GPIO_VALUE,
	DEVFS_GPIO_DIRECTION,
};

enum devfs_dir_level {
	DEVFS_DIR_ROOT,
	DEVFS_DIR_GPIO,
	DEVFS_DIR_PIN,
};

struct devfs_file {
	enum devfs_node_type type;
	uint8_t pin;
	bool read_eof;
};

static struct {
	int value;
	bool value_set;
	bool dir_out;
	bool dir_set;
} gpio_state[DEVFS_MAX_PIN + 1];

struct devfs_dir {
	enum devfs_dir_level level;
	uint8_t pin;
	uint8_t pos;
};

static const struct device *get_gpio_dev(uint8_t pin)
{
	if (pin <= 31) {
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio0));
	}
	return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio1));
}

static int parse_path(const char *path, enum devfs_node_type *type,
		      uint8_t *pin)
{
	unsigned int parsed_pin;
	int consumed = 0;

	if (sscanf(path, "/dev/gpio/%u/value%n", &parsed_pin, &consumed) == 1
	    && path[consumed] == '\0') {
		if (parsed_pin > DEVFS_MAX_PIN) {
			return -EINVAL;
		}
		*type = DEVFS_GPIO_VALUE;
		*pin = (uint8_t)parsed_pin;
		return 0;
	}

	if (sscanf(path, "/dev/gpio/%u/direction%n", &parsed_pin,
		    &consumed) == 1
	    && path[consumed] == '\0') {
		if (parsed_pin > DEVFS_MAX_PIN) {
			return -EINVAL;
		}
		*type = DEVFS_GPIO_DIRECTION;
		*pin = (uint8_t)parsed_pin;
		return 0;
	}

	return -ENOENT;
}

static int devfs_open(struct fs_file_t *filp, const char *fs_path,
		      fs_mode_t flags)
{
	struct devfs_file *df;
	enum devfs_node_type type;
	uint8_t pin;

	if (parse_path(fs_path, &type, &pin) != 0) {
		LOG_ERR("devfs: open parse fail '%s'", fs_path);
		return -ENOENT;
	}

	LOG_INF("devfs: open '%s' pin=%u type=%d", fs_path, pin, type);

	if (get_gpio_dev(pin) == NULL) {
		LOG_ERR("devfs: no gpio dev for pin %u", pin);
		return -ENODEV;
	}

	df = k_malloc(sizeof(*df));
	if (!df) {
		return -ENOMEM;
	}

	df->type = type;
	df->pin = pin;
	df->read_eof = false;
	filp->filep = df;
	LOG_INF("devfs: open ok pin=%u type=%d filep=%p", pin, type, df);
	return 0;
}

static ssize_t devfs_read(struct fs_file_t *filp, void *dest, size_t nbytes)
{
	struct devfs_file *df = filp->filep;
	const struct device *dev = get_gpio_dev(df->pin);
	char buf[16];
	int val, len;

	if (nbytes == 0) {
		return -EIO;
	}

	if (!dev) {
		LOG_WRN("devfs: no device for pin %u", df->pin);
		static const char mock_val[] = "0\n";
		size_t len = strlen(mock_val);
		if (len > nbytes) len = nbytes;
		memcpy(dest, mock_val, len);
		return (ssize_t)len;
	}

	if (df->read_eof) {
		return 0;
	}
	df->read_eof = true;

	switch (df->type) {
	case DEVFS_GPIO_VALUE:
		if (gpio_state[df->pin].value_set) {
			len = snprintf(buf, sizeof(buf), "%d\n",
				       gpio_state[df->pin].value);
		} else {
			len = snprintf(buf, sizeof(buf), "?\n");
		}
		break;
	case DEVFS_GPIO_DIRECTION:
		if (gpio_state[df->pin].dir_set) {
			len = snprintf(buf, sizeof(buf), "%s\n",
				       gpio_state[df->pin].dir_out ? "out" : "in");
		} else {
			len = snprintf(buf, sizeof(buf), "?\n");
		}
		break;
	default:
		return -EINVAL;
	}

	if ((size_t)len > nbytes) {
		len = (int)nbytes;
	}
	memcpy(dest, buf, len);
	return len;
}

static ssize_t devfs_write(struct fs_file_t *filp, const void *src,
			   size_t nbytes)
{
	struct devfs_file *df = filp->filep;
	const struct device *dev = get_gpio_dev(df->pin);
	char buf[16];
	gpio_flags_t flags;
	int ret;

	if (nbytes == 0) {
		return -EIO;
	}

	if (!dev) {
		LOG_WRN("devfs: no device for pin %u, accepting write", df->pin);
		return (ssize_t)nbytes;
	}

	size_t copy_len = (nbytes < sizeof(buf) - 1) ? nbytes : sizeof(buf) - 1;

	memcpy(buf, src, copy_len);
	buf[copy_len] = '\0';

	LOG_INF("devfs: write pin=%u type=%d nbytes=%zu copy_len=%zu",
		df->pin, df->type, nbytes, copy_len);
	for (size_t dbg = 0; dbg < copy_len && dbg < 16; dbg++) {
		LOG_INF("devfs:   buf[%zu]=0x%02x '%c'", dbg,
			(uint8_t)buf[dbg],
			buf[dbg] >= 0x20 && buf[dbg] < 0x7f ? buf[dbg]
							       : '.');
	}

	switch (df->type) {
	case DEVFS_GPIO_VALUE: {
		uint8_t val = (uint8_t)buf[0];
		int target;

		if (val == '1' || val == 1 || val == 'o') {
			target = 1;
		} else if (val == '0' || val == 0) {
			target = 0;
		} else {
			LOG_ERR("devfs: gpio%u bad val '0x%02x'", df->pin, val);
			return -EINVAL;
		}

		ret = gpio_pin_configure(dev, df->pin, GPIO_OUTPUT);
		if (ret < 0) {
			LOG_ERR("devfs: gpio%u configure out failed: %d",
				df->pin, ret);
			return ret;
		}
		ret = gpio_pin_set(dev, df->pin, target);
		if (ret < 0) {
			LOG_ERR("devfs: gpio%u set %d failed: %d", df->pin,
				target, ret);
			return ret;
		}
		gpio_state[df->pin].value = target;
		gpio_state[df->pin].value_set = true;
		LOG_INF("devfs: gpio%u set %d", df->pin, target);
		return (ssize_t)nbytes;
	}

	case DEVFS_GPIO_DIRECTION: {
		gpio_flags_t flags;

		bool dir_out;

		if (strncmp(buf, "out", 3) == 0 || buf[0] == 1) {
			flags = GPIO_OUTPUT;
			dir_out = true;
		} else if (strncmp(buf, "in", 2) == 0 || buf[0] == 0) {
			flags = GPIO_INPUT;
			dir_out = false;
		} else {
			LOG_ERR("devfs: gpio%u bad dir '%.*s'", df->pin,
				(int)copy_len, buf);
			return -EINVAL;
		}

		ret = gpio_pin_configure(dev, df->pin, flags);
		if (ret < 0) {
			LOG_ERR("devfs: gpio%u configure %s failed: %d",
				df->pin, dir_out ? "out" : "in", ret);
			return ret;
		}
		gpio_state[df->pin].dir_out = dir_out;
		gpio_state[df->pin].dir_set = true;
		LOG_INF("devfs: gpio%u dir %s", df->pin,
			dir_out ? "out" : "in");
		return (ssize_t)nbytes;
	}

	default:
		return -EINVAL;
	}
}

static int devfs_lseek(struct fs_file_t *filp, off_t off, int whence)
{
	return 0;
}

static off_t devfs_tell(struct fs_file_t *filp)
{
	return 0;
}

static int devfs_close(struct fs_file_t *filp)
{
	k_free(filp->filep);
	filp->filep = NULL;
	return 0;
}

static int classify_path(const char *path, enum devfs_dir_level *level,
			 uint8_t *pin)
{
	unsigned int parsed_pin;

	if (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0) {
		*level = DEVFS_DIR_ROOT;
		*pin = 0;
		return 0;
	}

	if (strcmp(path, "/dev/gpio") == 0 ||
	    strcmp(path, "/dev/gpio/") == 0) {
		*level = DEVFS_DIR_GPIO;
		*pin = 0;
		return 0;
	}

	{
		int n = 0;

		if (sscanf(path, "/dev/gpio/%u%n", &parsed_pin, &n) == 1 &&
		    parsed_pin <= DEVFS_MAX_PIN && path[n] == '\0') {
			*level = DEVFS_DIR_PIN;
			*pin = (uint8_t)parsed_pin;
			return 0;
		}
	}

	return -ENOENT;
}

static int devfs_stat(struct fs_mount_t *mountp, const char *path,
		      struct fs_dirent *entry)
{
	enum devfs_node_type type;
	uint8_t pin;

	if (parse_path(path, &type, &pin) == 0) {
		if (get_gpio_dev(pin) == NULL) {
			return -ENODEV;
		}
		entry->type = FS_DIR_ENTRY_FILE;
		entry->size = 0;
		return 0;
	}

	enum devfs_dir_level level;

	if (classify_path(path, &level, &pin) == 0) {
		entry->type = FS_DIR_ENTRY_DIR;
		entry->size = 0;
		snprintf(entry->name, sizeof(entry->name), "dev");
		return 0;
	}

	return -ENOENT;
}

static int devfs_opendir(struct fs_dir_t *dirp, const char *fs_path)
{
	struct devfs_dir *dd;
	enum devfs_dir_level level;
	uint8_t pin;

	if (classify_path(fs_path, &level, &pin) != 0) {
		return -ENOENT;
	}

	dd = k_malloc(sizeof(*dd));
	if (!dd) {
		return -ENOMEM;
	}

	dd->level = level;
	dd->pin = pin;
	dd->pos = 0;
	dirp->dirp = dd;
	return 0;
}

static int devfs_readdir(struct fs_dir_t *dirp, struct fs_dirent *entry)
{
	struct devfs_dir *dd = dirp->dirp;
	uint8_t pos = dd->pos++;

	switch (dd->level) {
	case DEVFS_DIR_ROOT:
		if (pos == 0) {
			entry->type = FS_DIR_ENTRY_DIR;
			entry->size = 0;
			strncpy(entry->name, "gpio",
				sizeof(entry->name) - 1);
			return 0;
		}
		break;
	case DEVFS_DIR_GPIO:
		if (pos <= DEVFS_MAX_PIN) {
			entry->type = FS_DIR_ENTRY_DIR;
			entry->size = 0;
			snprintf(entry->name, sizeof(entry->name),
				 "%u", pos);
			return 0;
		}
		break;
	case DEVFS_DIR_PIN:
		if (pos == 0) {
			entry->type = FS_DIR_ENTRY_FILE;
			entry->size = 0;
			strncpy(entry->name, "value",
				sizeof(entry->name) - 1);
			return 0;
		} else if (pos == 1) {
			entry->type = FS_DIR_ENTRY_FILE;
			entry->size = 0;
			strncpy(entry->name, "direction",
				sizeof(entry->name) - 1);
			return 0;
		}
		break;
	}

	entry->name[0] = '\0';
	return 0;
}

static int devfs_closedir(struct fs_dir_t *dirp)
{
	k_free(dirp->dirp);
	dirp->dirp = NULL;
	return 0;
}

static int devfs_do_mount(struct fs_mount_t *mountp)
{
	return 0;
}

static int devfs_unmount(struct fs_mount_t *mountp)
{
	return 0;
}

static struct fs_file_system_t devfs_ops = {
	.open = devfs_open,
	.read = devfs_read,
	.write = devfs_write,
	.lseek = devfs_lseek,
	.tell = devfs_tell,
	.close = devfs_close,
	.opendir = devfs_opendir,
	.readdir = devfs_readdir,
	.closedir = devfs_closedir,
	.stat = devfs_stat,
	.mount = devfs_do_mount,
	.unmount = devfs_unmount,
};

#define WEBOS_DEVFS_TYPE FS_TYPE_EXTERNAL_BASE

static struct fs_mount_t devfs_mount_pt = {
	.type = WEBOS_DEVFS_TYPE,
	.mnt_point = DEVFS_MOUNT_POINT,
	.fs_data = NULL,
};

int devfs_register(void)
{
	int ret = fs_register(WEBOS_DEVFS_TYPE, &devfs_ops);

	if (ret != 0) {
		LOG_ERR("devfs register failed: %d", ret);
		return ret;
	}

	ret = fs_mount(&devfs_mount_pt);
	if (ret != 0) {
		LOG_ERR("devfs mount failed: %d", ret);
		fs_unregister(WEBOS_DEVFS_TYPE, &devfs_ops);
		return ret;
	}

	LOG_INF("devfs mounted at %s", DEVFS_MOUNT_POINT);
	return 0;
}

int devfs_unregister(void)
{
	int ret = fs_unmount(&devfs_mount_pt);

	if (ret != 0) {
		return ret;
	}
	return fs_unregister(WEBOS_DEVFS_TYPE, &devfs_ops);
}
