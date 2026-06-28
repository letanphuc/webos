#include <errno.h>
#include <string.h>

#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "services/fs/fs.h"

LOG_MODULE_REGISTER(webos_fs, LOG_LEVEL_INF);

static FATFS webos_fatfs;
static struct fs_mount_t webos_mount = {
	.type = FS_FATFS,
	.mnt_point = WEBOS_MOUNT_POINT,
	.fs_data = &webos_fatfs,
	.storage_dev = (void *)"RAM",
	.flags = FS_MOUNT_FLAG_USE_DISK_ACCESS,
};

bool webos_path_allowed(const char *path)
{
	return strncmp(path, WEBOS_MOUNT_POINT "/", strlen(WEBOS_MOUNT_POINT "/")) == 0 &&
	       strstr(path, "..") == NULL;
}

int ensure_dir(const char *path)
{
	int ret = fs_mkdir(path);

	return ret == -EEXIST ? 0 : ret;
}

int write_file(const char *path, const char *data)
{
	struct fs_file_t file;
	int ret;

	if (!webos_path_allowed(path)) {
		return -EINVAL;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_TRUNC | FS_O_WRITE);
	if (ret != 0) {
		return ret;
	}

	ret = fs_write(&file, data, strlen(data));
	if (ret >= 0 && ret != strlen(data)) {
		ret = -EIO;
	}

	int close_ret = fs_close(&file);
	return ret < 0 ? ret : close_ret;
}

void init_filesystem_layout(void)
{
	int ret;

	ret = fs_mount(&webos_mount);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("Filesystem mount failed: %d", ret);
		return;
	}

	ret = ensure_dir(WEBOS_MOUNT_POINT "/apps");
	if (ret != 0) {
		LOG_ERR("/apps init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/config");
	if (ret != 0) {
		LOG_ERR("/config init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/logs");
	if (ret != 0) {
		LOG_ERR("/logs init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/ota");
	if (ret != 0) {
		LOG_ERR("/ota init failed: %d", ret);
	}
	ret = ensure_dir(WEBOS_MOUNT_POINT "/www");
	if (ret != 0) {
		LOG_ERR("/www init failed: %d", ret);
	}
}
