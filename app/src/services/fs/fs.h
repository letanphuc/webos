#ifndef WEBOS_SERVICES_FS_H
#define WEBOS_SERVICES_FS_H

#include <stdbool.h>

#define WEBOS_MOUNT_POINT "/RAM:"

bool webos_path_allowed(const char *path);
int ensure_dir(const char *path);
int write_file(const char *path, const char *data);
void init_filesystem_layout(void);

#endif
