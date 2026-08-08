#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include "devfs.h"

static const struct devfs_file_ops test_ops = {};

static void assert_entry(struct fs_dir_t* dir, const char* name, enum fs_dir_entry_type type) {
  struct fs_dirent entry;

  zassert_ok(fs_readdir(dir, &entry));
  zassert_equal(entry.type, type);
  zassert_equal(strcmp(entry.name, name), 0, "expected %s, got %s", name, entry.name);
}

static void* devfs_setup(void) {
  zassert_ok(devfs_register());
  return NULL;
}

ZTEST(devfs, test_tree_builds_and_prunes_implicit_directories) {
  struct fs_dirent entry;

  zassert_ok(devfs_register_file("/devices/gpio/2/value", &test_ops, NULL));
  zassert_ok(fs_stat("/dev/devices", &entry));
  zassert_equal(entry.type, FS_DIR_ENTRY_DIR);
  zassert_ok(fs_stat("/dev/devices/gpio/2", &entry));
  zassert_equal(entry.type, FS_DIR_ENTRY_DIR);
  zassert_ok(fs_stat("/dev/devices/gpio/2/value", &entry));
  zassert_equal(entry.type, FS_DIR_ENTRY_FILE);

  zassert_ok(devfs_unregister_file("/devices/gpio/2/value"));
  zassert_equal(fs_stat("/dev/devices", &entry), -ENOENT);
}

ZTEST(devfs, test_directory_listing_is_direct_and_sorted) {
  struct fs_dir_t dir;
  struct fs_dirent end;

  zassert_ok(devfs_register_file("/zeta/value", &test_ops, NULL));
  zassert_ok(devfs_register_file("/alpha/value", &test_ops, NULL));
  zassert_ok(devfs_register_file("/alpha/mode", &test_ops, NULL));

  fs_dir_t_init(&dir);
  zassert_ok(fs_opendir(&dir, "/dev"));
  assert_entry(&dir, "alpha", FS_DIR_ENTRY_DIR);
  assert_entry(&dir, "zeta", FS_DIR_ENTRY_DIR);
  zassert_ok(fs_readdir(&dir, &end));
  zassert_equal(end.name[0], '\0');
  zassert_ok(fs_closedir(&dir));

  fs_dir_t_init(&dir);
  zassert_ok(fs_opendir(&dir, "/dev/alpha/"));
  assert_entry(&dir, "mode", FS_DIR_ENTRY_FILE);
  assert_entry(&dir, "value", FS_DIR_ENTRY_FILE);
  zassert_ok(fs_closedir(&dir));

  zassert_ok(devfs_unregister_file("/alpha/mode"));
  zassert_ok(devfs_unregister_file("/alpha/value"));
  zassert_ok(devfs_unregister_file("/zeta/value"));
}

ZTEST(devfs, test_registration_owns_path_and_rejects_invalid_paths) {
  char path[] = "/dynamic/value";
  struct fs_dirent entry;

  zassert_ok(devfs_register_file(path, &test_ops, NULL));
  memset(path, 'x', sizeof(path) - 1);
  zassert_ok(fs_stat("/dev/dynamic/value", &entry));
  zassert_ok(devfs_unregister_file("/dynamic/value"));

  zassert_equal(devfs_register_file("dynamic/value", &test_ops, NULL), -EINVAL);
  zassert_equal(devfs_register_file("/dynamic//value", &test_ops, NULL), -EINVAL);
  zassert_equal(devfs_register_file("/dynamic/../value", &test_ops, NULL), -EINVAL);
  zassert_equal(devfs_register_file("/dynamic/value/", &test_ops, NULL), -EINVAL);
}

ZTEST(devfs, test_open_file_cannot_be_unregistered) {
  struct fs_file_t file;

  zassert_ok(devfs_register_file("/busy/value", &test_ops, NULL));
  fs_file_t_init(&file);
  zassert_ok(fs_open(&file, "/dev/busy/value", FS_O_READ));
  zassert_equal(devfs_unregister_file("/busy/value"), -EBUSY);
  zassert_ok(fs_close(&file));
  zassert_ok(devfs_unregister_file("/busy/value"));
}

ZTEST_SUITE(devfs, NULL, devfs_setup, NULL, NULL, NULL);
