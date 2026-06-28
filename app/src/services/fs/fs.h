#ifndef WEBOS_SERVICES_FS_H
#define WEBOS_SERVICES_FS_H

#include <stdbool.h>
#include <stddef.h>

#define WEBOS_MOUNT_POINT "/STORAGE:"

bool webos_path_allowed(const char *path);
int ensure_dir(const char *path);
int write_file(const char *path, const char *data);
int write_file_bin(const char *path, const uint8_t *data, size_t data_len);
void init_filesystem_layout(void);

#endif
