#include "devfs.h"

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(devfs, CONFIG_FS_LOG_LEVEL);

#define DEVFS_MOUNT_POINT "/dev"
#define DEVFS_MAX_FILES CONFIG_WEBOS_DEVFS_MAX_FILES
#define DEVFS_MAX_DIRS CONFIG_WEBOS_DEVFS_MAX_DIRS

struct devfs_node {
  const char* path;
  const struct devfs_file_ops* ops;
  void* user_data;
};

struct devfs_file {
  const struct devfs_node* node;
  void* data;
};

struct devfs_dir {
  char path[CONFIG_WEBOS_DEVFS_MAX_PATH_LEN];
  uint16_t pos;
};

static struct devfs_node nodes[DEVFS_MAX_FILES];
static struct k_mutex nodes_lock;

void* devfs_file_data(struct devfs_file* file) { return file->data; }

void devfs_file_set_data(struct devfs_file* file, void* data) { file->data = data; }

static const char* normalize_path(const char* path) {
  if (strcmp(path, DEVFS_MOUNT_POINT) == 0 || strcmp(path, DEVFS_MOUNT_POINT "/") == 0) {
    return "/";
  }

  if (strncmp(path, DEVFS_MOUNT_POINT "/", strlen(DEVFS_MOUNT_POINT) + 1) == 0) {
    return path + strlen(DEVFS_MOUNT_POINT);
  }

  return path;
}

static const struct devfs_node* find_node(const char* path) {
  const char* rel = normalize_path(path);

  for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
    if (nodes[i].path != NULL && strcmp(nodes[i].path, rel) == 0) {
      return &nodes[i];
    }
  }

  return NULL;
}

static bool is_direct_child(const char* dir, const char* path, const char** name) {
  const char* child;

  if (strcmp(dir, "/") == 0) {
    if (path[0] != '/') {
      return false;
    }
    child = path + 1;
  } else {
    size_t dir_len = strlen(dir);

    if (strncmp(path, dir, dir_len) != 0 || path[dir_len] != '/') {
      return false;
    }
    child = path + dir_len + 1;
  }

  if (child[0] == '\0' || strchr(child, '/') != NULL) {
    return false;
  }

  *name = child;
  return true;
}

static bool has_child_dir(const char* dir, const char* path, const char** name, size_t* name_len) {
  const char* child;
  const char* slash;

  if (strcmp(dir, "/") == 0) {
    if (path[0] != '/') {
      return false;
    }
    child = path + 1;
  } else {
    size_t dir_len = strlen(dir);

    if (strncmp(path, dir, dir_len) != 0 || path[dir_len] != '/') {
      return false;
    }
    child = path + dir_len + 1;
  }

  slash = strchr(child, '/');
  if (child[0] == '\0' || slash == NULL || slash == child) {
    return false;
  }

  *name = child;
  *name_len = (size_t)(slash - child);
  return true;
}

static bool dir_exists(const char* path) {
  const char* dir = normalize_path(path);

  if (strcmp(dir, "/") == 0 || strcmp(path, DEVFS_MOUNT_POINT) == 0) {
    return true;
  }

  for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
    const char* name;
    size_t name_len;

    if (nodes[i].path != NULL &&
        (has_child_dir(dir, nodes[i].path, &name, &name_len) || is_direct_child(dir, nodes[i].path, &name))) {
      return true;
    }
  }

  return false;
}

static int devfs_open(struct fs_file_t* filp, const char* fs_path, fs_mode_t flags) {
  const struct devfs_node* node;
  struct devfs_file* file;
  int ret;

  k_mutex_lock(&nodes_lock, K_FOREVER);
  node = find_node(fs_path);
  k_mutex_unlock(&nodes_lock);
  if (node == NULL) {
    return -ENOENT;
  }

  file = k_malloc(sizeof(*file));
  if (file == NULL) {
    return -ENOMEM;
  }

  file->node = node;
  file->data = NULL;
  if (node->ops->open != NULL) {
    ret = node->ops->open(file, node->user_data, flags);
    if (ret != 0) {
      k_free(file);
      return ret;
    }
  }

  filp->filep = file;
  return 0;
}

static ssize_t devfs_read(struct fs_file_t* filp, void* dest, size_t nbytes) {
  struct devfs_file* file = filp->filep;

  if (file->node->ops->read == NULL) {
    return -ENOTSUP;
  }

  return file->node->ops->read(file, dest, nbytes);
}

static ssize_t devfs_write(struct fs_file_t* filp, const void* src, size_t nbytes) {
  struct devfs_file* file = filp->filep;

  if (file->node->ops->write == NULL) {
    return -ENOTSUP;
  }

  return file->node->ops->write(file, src, nbytes);
}

static int devfs_lseek(struct fs_file_t* filp, off_t off, int whence) { return 0; }

static off_t devfs_tell(struct fs_file_t* filp) { return 0; }

static int devfs_close(struct fs_file_t* filp) {
  struct devfs_file* file = filp->filep;
  int ret = 0;

  if (file->node->ops->close != NULL) {
    ret = file->node->ops->close(file);
  }
  k_free(file);
  filp->filep = NULL;
  return ret;
}

static int devfs_stat(struct fs_mount_t* mountp, const char* path, struct fs_dirent* entry) {
  k_mutex_lock(&nodes_lock, K_FOREVER);
  if (find_node(path) != NULL) {
    k_mutex_unlock(&nodes_lock);
    entry->type = FS_DIR_ENTRY_FILE;
    entry->size = 0;
    return 0;
  }

  if (dir_exists(path)) {
    k_mutex_unlock(&nodes_lock);
    entry->type = FS_DIR_ENTRY_DIR;
    entry->size = 0;
    return 0;
  }
  k_mutex_unlock(&nodes_lock);

  return -ENOENT;
}

static int devfs_opendir(struct fs_dir_t* dirp, const char* fs_path) {
  struct devfs_dir* dir;
  const char* rel = normalize_path(fs_path);

  k_mutex_lock(&nodes_lock, K_FOREVER);
  if (!dir_exists(fs_path)) {
    k_mutex_unlock(&nodes_lock);
    return -ENOENT;
  }
  k_mutex_unlock(&nodes_lock);

  dir = k_malloc(sizeof(*dir));
  if (dir == NULL) {
    return -ENOMEM;
  }

  strncpy(dir->path, rel, sizeof(dir->path) - 1);
  dir->path[sizeof(dir->path) - 1] = '\0';
  dir->pos = 0;
  dirp->dirp = dir;
  return 0;
}

static int devfs_readdir(struct fs_dir_t* dirp, struct fs_dirent* entry) {
  struct devfs_dir* dir = dirp->dirp;
  uint16_t seen_dirs = 0;
  uint16_t pos = 0;
  const char* dir_names[DEVFS_MAX_DIRS];
  size_t dir_name_lens[DEVFS_MAX_DIRS];

  entry->name[0] = '\0';
  k_mutex_lock(&nodes_lock, K_FOREVER);
  for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
    const char* name;
    size_t name_len;
    bool duplicate = false;

    if (nodes[i].path == NULL || !has_child_dir(dir->path, nodes[i].path, &name, &name_len)) {
      continue;
    }

    for (uint16_t j = 0; j < seen_dirs; j++) {
      if (dir_name_lens[j] == name_len && strncmp(dir_names[j], name, name_len) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    if (seen_dirs < ARRAY_SIZE(dir_names)) {
      dir_names[seen_dirs] = name;
      dir_name_lens[seen_dirs] = name_len;
      seen_dirs++;
    }

    if (pos++ == dir->pos) {
      entry->type = FS_DIR_ENTRY_DIR;
      entry->size = 0;
      snprintk(entry->name, sizeof(entry->name), "%.*s", (int)name_len, name);
      dir->pos++;
      k_mutex_unlock(&nodes_lock);
      return 0;
    }
  }

  for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
    const char* name;

    if (nodes[i].path == NULL || !is_direct_child(dir->path, nodes[i].path, &name)) {
      continue;
    }

    if (pos++ == dir->pos) {
      entry->type = FS_DIR_ENTRY_FILE;
      entry->size = 0;
      strncpy(entry->name, name, sizeof(entry->name) - 1);
      entry->name[sizeof(entry->name) - 1] = '\0';
      dir->pos++;
      k_mutex_unlock(&nodes_lock);
      return 0;
    }
  }
  k_mutex_unlock(&nodes_lock);

  return 0;
}

static int devfs_closedir(struct fs_dir_t* dirp) {
  k_free(dirp->dirp);
  dirp->dirp = NULL;
  return 0;
}

static int devfs_do_mount(struct fs_mount_t* mountp) { return 0; }

static int devfs_unmount(struct fs_mount_t* mountp) { return 0; }

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

int devfs_register_file(const char* path, const struct devfs_file_ops* ops, void* user_data) {
  int free_idx = -1;

  if (path == NULL || path[0] != '/' || ops == NULL) {
    return -EINVAL;
  }

  k_mutex_lock(&nodes_lock, K_FOREVER);
  for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
    if (nodes[i].path != NULL && strcmp(nodes[i].path, path) == 0) {
      k_mutex_unlock(&nodes_lock);
      return -EEXIST;
    }
    if (nodes[i].path == NULL && free_idx < 0) {
      free_idx = (int)i;
    }
  }

  if (free_idx < 0) {
    k_mutex_unlock(&nodes_lock);
    return -ENOMEM;
  }

  nodes[free_idx].path = path;
  nodes[free_idx].ops = ops;
  nodes[free_idx].user_data = user_data;
  k_mutex_unlock(&nodes_lock);
  return 0;
}

int devfs_unregister_file(const char* path) {
  k_mutex_lock(&nodes_lock, K_FOREVER);
  for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
    if (nodes[i].path != NULL && strcmp(nodes[i].path, path) == 0) {
      nodes[i].path = NULL;
      nodes[i].ops = NULL;
      nodes[i].user_data = NULL;
      k_mutex_unlock(&nodes_lock);
      return 0;
    }
  }
  k_mutex_unlock(&nodes_lock);
  return -ENOENT;
}

int devfs_register(void) {
  int ret;

  k_mutex_init(&nodes_lock);
  ret = fs_register(WEBOS_DEVFS_TYPE, &devfs_ops);
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

int devfs_unregister(void) {
  int ret = fs_unmount(&devfs_mount_pt);

  if (ret != 0) {
    return ret;
  }
  return fs_unregister(WEBOS_DEVFS_TYPE, &devfs_ops);
}
