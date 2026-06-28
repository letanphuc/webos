#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "services/fs/fs.h"

LOG_MODULE_REGISTER(webos_fs, LOG_LEVEL_INF);

bool webos_path_allowed(const char *path)
{
	return strncmp(path, WEBOS_MOUNT_POINT "/", strlen(WEBOS_MOUNT_POINT "/")) == 0 &&
	       strstr(path, "..") == NULL;
}

int ensure_dir(const char *path)
{
	struct fs_dirent entry;
	int ret;

	ret = fs_stat(path, &entry);
	if (ret == 0) {
		return 0;
	}

	ret = fs_mkdir(path);
	if (ret != 0) {
		LOG_ERR("ensure_dir %s failed: %d", path, ret);
	} else {
		LOG_INF("ensure_dir %s: created", path);
	}
	return ret;
}

static int ensure_parent_dirs(const char *path)
{
	char parent[128];
	const char *last_slash = strrchr(path, '/');
	size_t parent_len;
	size_t mount_len = strlen(WEBOS_MOUNT_POINT);

	if (last_slash == NULL || last_slash == path) {
		return 0;
	}

	parent_len = last_slash - path;
	if (parent_len >= sizeof(parent)) {
		return -ENAMETOOLONG;
	}

	memcpy(parent, path, parent_len);
	parent[parent_len] = '\0';

	if (strncmp(parent, WEBOS_MOUNT_POINT, mount_len) != 0) {
		return -EINVAL;
	}

	for (char *slash = parent + mount_len; *slash != '\0'; slash++) {
		if (*slash != '/') {
			continue;
		}

		*slash = '\0';
		if (strlen(parent) > mount_len) {
			int ret = ensure_dir(parent);

			if (ret != 0) {
				*slash = '/';
				return ret;
			}
		}
		*slash = '/';
	}

	return ensure_dir(parent);
}

int write_file(const char *path, const char *data)
{
	size_t len = strlen(data);

	return write_file_bin(path, (const uint8_t *)data, len);
}

int write_file_bin(const char *path, const uint8_t *data, size_t data_len)
{
	struct fs_file_t file;
	int ret;

	if (!webos_path_allowed(path)) {
		LOG_ERR("write_file_bin: path not allowed: %s", path);
		return -EINVAL;
	}

	ret = ensure_parent_dirs(path);
	if (ret != 0) {
		LOG_ERR("write_file_bin: ensure_parent_dirs(%s) failed: %d", path, ret);
		return ret;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_RDWR);
	if (ret != 0) {
		LOG_ERR("write_file_bin: fs_open(%s) failed: %d", path, ret);
		return ret;
	}

	ret = fs_write(&file, data, data_len);
	if (ret >= 0 && (size_t)ret != data_len) {
		ret = -EIO;
	}

	int close_ret = fs_close(&file);

	return ret < 0 ? ret : close_ret;
}

void init_filesystem_layout(void)
{
	struct fs_dirent dirent;
	int ret;
	int attempt;

	LOG_INF("Initializing filesystem layout under %s", WEBOS_MOUNT_POINT);

	for (attempt = 0; attempt < 20; attempt++) {
		ret = fs_stat(WEBOS_MOUNT_POINT, &dirent);
		if (ret == 0) {
			break;
		}
		LOG_DBG("Waiting for %s to be ready (attempt %d, ret=%d)",
			WEBOS_MOUNT_POINT, attempt + 1, ret);
		k_sleep(K_MSEC(500));
	}
	if (ret != 0) {
		LOG_ERR("Filesystem %s not ready after %d attempts: %d",
			WEBOS_MOUNT_POINT, attempt, ret);
		return;
	}
	LOG_INF("Filesystem %s is ready", WEBOS_MOUNT_POINT);

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
