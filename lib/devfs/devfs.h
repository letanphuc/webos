#ifndef WEBOS_DEVFS_H
#define WEBOS_DEVFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

struct devfs_file;

struct devfs_file_ops {
  int (*open)(struct devfs_file* file, void* user_data, int flags);
  ssize_t (*read)(struct devfs_file* file, void* dest, size_t nbytes);
  ssize_t (*write)(struct devfs_file* file, const void* src, size_t nbytes);
  int (*close)(struct devfs_file* file);
};

void* devfs_file_data(struct devfs_file* file);
void devfs_file_set_data(struct devfs_file* file, void* data);

int devfs_register(void);
int devfs_unregister(void);
int devfs_register_file(const char* path, const struct devfs_file_ops* ops, void* user_data);
int devfs_unregister_file(const char* path);

#ifdef __cplusplus
}
#endif

#endif
