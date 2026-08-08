#include "devfs.h"

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(devfs, CONFIG_FS_LOG_LEVEL);

#define DEVFS_MOUNT_POINT "/dev"

enum devfs_node_type {
  DEVFS_NODE_DIR,
  DEVFS_NODE_FILE,
};

struct devfs_node {
  sys_dnode_t tree_node;
  sys_dlist_t children;
  struct devfs_node* parent;
  struct devfs_file_ops ops;
  void* user_data;
  uint16_t open_count;
  enum devfs_node_type type;
  char name[];
};

struct devfs_file {
  struct devfs_node* node;
  void* data;
};

struct devfs_dir {
  struct devfs_node* node;
  char last_name[CONFIG_WEBOS_DEVFS_MAX_PATH_LEN];
  bool started;
};

static struct devfs_node root = {
    .children = SYS_DLIST_STATIC_INIT(&root.children),
    .type = DEVFS_NODE_DIR,
};
K_MUTEX_DEFINE(tree_lock);

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

static int validate_path(const char* path, bool allow_trailing_slash, const char** relative) {
  const char* rel;
  const char* component;
  size_t len;

  if (path == NULL) {
    return -EINVAL;
  }

  rel = normalize_path(path);
  len = strlen(rel);
  if (len == 0 || len >= CONFIG_WEBOS_DEVFS_MAX_PATH_LEN || rel[0] != '/') {
    return -EINVAL;
  }

  if (!allow_trailing_slash && len > 1 && rel[len - 1] == '/') {
    return -EINVAL;
  }

  component = rel + 1;
  while (*component != '\0') {
    const char* slash = strchr(component, '/');
    size_t component_len = slash == NULL ? strlen(component) : (size_t)(slash - component);

    if (component_len == 0 || (component_len == 1 && component[0] == '.') ||
        (component_len == 2 && component[0] == '.' && component[1] == '.')) {
      return -EINVAL;
    }
    component = slash == NULL ? component + component_len : slash + 1;
  }

  *relative = rel;
  return 0;
}

static struct devfs_node* find_child(struct devfs_node* parent, const char* name, size_t name_len) {
  struct devfs_node* child;

  SYS_DLIST_FOR_EACH_CONTAINER(&parent->children, child, tree_node) {
    int cmp = strncmp(child->name, name, name_len);

    if (cmp == 0 && child->name[name_len] == '\0') {
      return child;
    }
    if (cmp > 0 || (cmp == 0 && child->name[name_len] != '\0')) {
      break;
    }
  }

  return NULL;
}

static struct devfs_node* alloc_node(enum devfs_node_type type, const char* name, size_t name_len) {
  struct devfs_node* node = k_malloc(sizeof(*node) + name_len + 1);

  if (node == NULL) {
    return NULL;
  }

  memset(node, 0, sizeof(*node));
  sys_dlist_init(&node->children);
  node->type = type;
  memcpy(node->name, name, name_len);
  node->name[name_len] = '\0';
  return node;
}

static void insert_child(struct devfs_node* parent, struct devfs_node* node) {
  struct devfs_node* child;

  node->parent = parent;
  SYS_DLIST_FOR_EACH_CONTAINER(&parent->children, child, tree_node) {
    if (strcmp(node->name, child->name) < 0) {
      sys_dlist_insert(&child->tree_node, &node->tree_node);
      return;
    }
  }
  sys_dlist_append(&parent->children, &node->tree_node);
}

static void unlink_child(struct devfs_node* node) {
  sys_dlist_remove(&node->tree_node);
  node->parent = NULL;
}

static void prune_empty_dirs(struct devfs_node* node) {
  while (node != &root && node->type == DEVFS_NODE_DIR && sys_dlist_is_empty(&node->children) &&
         node->open_count == 0) {
    struct devfs_node* parent = node->parent;

    unlink_child(node);
    k_free(node);
    node = parent;
  }
}

static struct devfs_node* find_node(const char* path) {
  const char* rel;
  const char* component;
  struct devfs_node* node = &root;

  if (validate_path(path, true, &rel) != 0) {
    return NULL;
  }
  if (strcmp(rel, "/") == 0) {
    return &root;
  }

  component = rel + 1;
  while (*component != '\0') {
    const char* slash = strchr(component, '/');
    size_t component_len = slash == NULL ? strlen(component) : (size_t)(slash - component);

    node = find_child(node, component, component_len);
    if (node == NULL || (slash != NULL && node->type != DEVFS_NODE_DIR)) {
      return NULL;
    }
    component = slash == NULL ? component + component_len : slash + 1;
  }

  return node;
}

static int devfs_open(struct fs_file_t* filp, const char* fs_path, fs_mode_t flags) {
  struct devfs_node* node;
  struct devfs_file* file;
  int ret;

  file = k_malloc(sizeof(*file));
  if (file == NULL) {
    return -ENOMEM;
  }

  k_mutex_lock(&tree_lock, K_FOREVER);
  node = find_node(fs_path);
  if (node == NULL) {
    ret = -ENOENT;
  } else if (node->type != DEVFS_NODE_FILE) {
    ret = -EISDIR;
  } else {
    node->open_count++;
    ret = 0;
  }
  k_mutex_unlock(&tree_lock);
  if (ret != 0) {
    k_free(file);
    return ret;
  }

  file->node = node;
  file->data = NULL;
  if (node->ops.open != NULL) {
    ret = node->ops.open(file, node->user_data, flags);
    if (ret != 0) {
      k_mutex_lock(&tree_lock, K_FOREVER);
      node->open_count--;
      k_mutex_unlock(&tree_lock);
      k_free(file);
      return ret;
    }
  }

  filp->filep = file;
  return 0;
}

static ssize_t devfs_read(struct fs_file_t* filp, void* dest, size_t nbytes) {
  struct devfs_file* file = filp->filep;

  if (file->node->ops.read == NULL) {
    return -ENOTSUP;
  }

  return file->node->ops.read(file, dest, nbytes);
}

static ssize_t devfs_write(struct fs_file_t* filp, const void* src, size_t nbytes) {
  struct devfs_file* file = filp->filep;

  if (file->node->ops.write == NULL) {
    return -ENOTSUP;
  }

  return file->node->ops.write(file, src, nbytes);
}

static int devfs_lseek(struct fs_file_t* filp, off_t off, int whence) { return 0; }

static off_t devfs_tell(struct fs_file_t* filp) { return 0; }

static int devfs_close(struct fs_file_t* filp) {
  struct devfs_file* file = filp->filep;
  int ret = 0;

  if (file->node->ops.close != NULL) {
    ret = file->node->ops.close(file);
  }

  k_mutex_lock(&tree_lock, K_FOREVER);
  file->node->open_count--;
  k_mutex_unlock(&tree_lock);
  k_free(file);
  filp->filep = NULL;
  return ret;
}

static int devfs_stat(struct fs_mount_t* mountp, const char* path, struct fs_dirent* entry) {
  struct devfs_node* node;

  k_mutex_lock(&tree_lock, K_FOREVER);
  node = find_node(path);
  if (node != NULL) {
    entry->type = node->type == DEVFS_NODE_DIR ? FS_DIR_ENTRY_DIR : FS_DIR_ENTRY_FILE;
    entry->size = 0;
  }
  k_mutex_unlock(&tree_lock);

  return node == NULL ? -ENOENT : 0;
}

static int devfs_opendir(struct fs_dir_t* dirp, const char* fs_path) {
  struct devfs_dir* dir;
  struct devfs_node* node;
  int ret;

  dir = k_malloc(sizeof(*dir));
  if (dir == NULL) {
    return -ENOMEM;
  }

  k_mutex_lock(&tree_lock, K_FOREVER);
  node = find_node(fs_path);
  if (node == NULL) {
    ret = -ENOENT;
  } else if (node->type != DEVFS_NODE_DIR) {
    ret = -ENOTDIR;
  } else {
    node->open_count++;
    ret = 0;
  }
  k_mutex_unlock(&tree_lock);
  if (ret != 0) {
    k_free(dir);
    return ret;
  }

  dir->node = node;
  dir->last_name[0] = '\0';
  dir->started = false;
  dirp->dirp = dir;
  return 0;
}

static int devfs_readdir(struct fs_dir_t* dirp, struct fs_dirent* entry) {
  struct devfs_dir* dir = dirp->dirp;
  struct devfs_node* child;

  entry->name[0] = '\0';
  k_mutex_lock(&tree_lock, K_FOREVER);
  SYS_DLIST_FOR_EACH_CONTAINER(&dir->node->children, child, tree_node) {
    if (dir->started && strcmp(child->name, dir->last_name) <= 0) {
      continue;
    }

    entry->type = child->type == DEVFS_NODE_DIR ? FS_DIR_ENTRY_DIR : FS_DIR_ENTRY_FILE;
    entry->size = 0;
    strncpy(entry->name, child->name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    strncpy(dir->last_name, child->name, sizeof(dir->last_name) - 1);
    dir->last_name[sizeof(dir->last_name) - 1] = '\0';
    dir->started = true;
    break;
  }
  k_mutex_unlock(&tree_lock);

  return 0;
}

static int devfs_closedir(struct fs_dir_t* dirp) {
  struct devfs_dir* dir = dirp->dirp;

  k_mutex_lock(&tree_lock, K_FOREVER);
  dir->node->open_count--;
  prune_empty_dirs(dir->node);
  k_mutex_unlock(&tree_lock);
  k_free(dir);
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
  const char* rel;
  const char* component;
  struct devfs_node* parent = &root;
  int ret;

  if (ops == NULL) {
    return -EINVAL;
  }

  ret = validate_path(path, false, &rel);
  if (ret != 0 || strcmp(rel, "/") == 0) {
    return -EINVAL;
  }

  k_mutex_lock(&tree_lock, K_FOREVER);
  component = rel + 1;
  while (*component != '\0') {
    const char* slash = strchr(component, '/');
    size_t component_len = slash == NULL ? strlen(component) : (size_t)(slash - component);
    enum devfs_node_type type = slash == NULL ? DEVFS_NODE_FILE : DEVFS_NODE_DIR;
    struct devfs_node* node = find_child(parent, component, component_len);

    if (node != NULL) {
      if (slash == NULL) {
        ret = node->type == DEVFS_NODE_FILE ? -EEXIST : -EISDIR;
        goto out;
      }
      if (node->type != DEVFS_NODE_DIR) {
        ret = -ENOTDIR;
        goto out;
      }
    } else {
      node = alloc_node(type, component, component_len);
      if (node == NULL) {
        ret = -ENOMEM;
        goto out;
      }
      insert_child(parent, node);
    }

    parent = node;
    component = slash == NULL ? component + component_len : slash + 1;
  }

  parent->ops = *ops;
  parent->user_data = user_data;
  ret = 0;

out:
  if (ret != 0) {
    prune_empty_dirs(parent);
  }
  k_mutex_unlock(&tree_lock);
  return ret;
}

int devfs_unregister_file(const char* path) {
  struct devfs_node* node;
  struct devfs_node* parent;
  int ret;

  k_mutex_lock(&tree_lock, K_FOREVER);
  node = find_node(path);
  if (node == NULL) {
    ret = -ENOENT;
  } else if (node->type != DEVFS_NODE_FILE) {
    ret = -EISDIR;
  } else if (node->open_count != 0) {
    ret = -EBUSY;
  } else {
    parent = node->parent;
    unlink_child(node);
    k_free(node);
    prune_empty_dirs(parent);
    ret = 0;
  }
  k_mutex_unlock(&tree_lock);
  return ret;
}

int devfs_register(void) {
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

int devfs_unregister(void) {
  int ret = fs_unmount(&devfs_mount_pt);

  if (ret != 0) {
    return ret;
  }
  return fs_unregister(WEBOS_DEVFS_TYPE, &devfs_ops);
}
