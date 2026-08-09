/*
 * Legacy ADB sync SEND service for WebOS.
 * Protocol behavior is derived from Android system/core/adb (Apache-2.0).
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "adb_service.h"

LOG_MODULE_DECLARE(webos_adb, LOG_LEVEL_INF);

#define SYNC_ID_STAT 0x54415453U
#define SYNC_ID_SEND 0x444e4553U
#define SYNC_ID_DATA 0x41544144U
#define SYNC_ID_DONE 0x454e4f44U
#define SYNC_ID_QUIT 0x54495551U
#define SYNC_ID_OKAY 0x59414b4fU
#define SYNC_ID_FAIL 0x4c494146U

#define SYNC_ROOT "/STORAGE:/"
#define SYNC_SIDE_SUFFIX_LEN 20U
#define SYNC_SPEC_CAPACITY (CONFIG_WEBOS_ADB_SYNC_PATH_MAX + 16U)
#define SYNC_FAIL_CAPACITY 96U

enum sync_state {
  SYNC_REQUEST_HEADER,
  SYNC_REQUEST_SPEC,
  SYNC_STAT_PATH,
  SYNC_SEND_HEADER,
  SYNC_SEND_DATA,
  SYNC_DRAIN_HEADER,
  SYNC_DRAIN_DATA,
  SYNC_DISCARD_AND_CLOSE,
};

struct sync_context {
  struct webos_adb_stream* stream;
  struct fs_file_t file;
  enum sync_state state;
  uint8_t header[8];
  size_t header_used;
  uint32_t remaining;
  char spec[SYNC_SPEC_CAPACITY];
  size_t spec_used;
  char path[CONFIG_WEBOS_ADB_SYNC_PATH_MAX];
  char temporary[CONFIG_WEBOS_ADB_SYNC_PATH_MAX + SYNC_SIDE_SUFFIX_LEN];
  char backup[CONFIG_WEBOS_ADB_SYNC_PATH_MAX + SYNC_SIDE_SUFFIX_LEN];
  bool file_open;
  bool temporary_live;
};

static struct sync_context contexts[CONFIG_WEBOS_ADB_MAX_STREAMS];

static struct sync_context* find_context(struct webos_adb_stream* stream) {
  for (size_t i = 0; i < ARRAY_SIZE(contexts); ++i) {
    if (contexts[i].stream == stream) {
      return &contexts[i];
    }
  }
  return NULL;
}

static bool path_exists(const char* path, struct fs_dirent* entry) { return fs_stat(path, entry) == 0; }

static bool sync_path_allowed(const char* path) {
  const size_t root_len = sizeof(SYNC_ROOT) - 1U;
  const size_t len = strlen(path);

  if (len <= root_len || len >= CONFIG_WEBOS_ADB_SYNC_PATH_MAX || strncmp(path, SYNC_ROOT, root_len) != 0 ||
      path[len - 1U] == '/' || strstr(path, ".adb-part-") != NULL || strstr(path, ".adb-old-") != NULL) {
    return false;
  }

  /* Reject ambiguous FAT paths instead of attempting to normalize them. */
  for (const char* p = path + root_len; *p != '\0'; ++p) {
    if ((unsigned char)*p < 0x20U || *p == '\\' || *p == ':' || (*p == '/' && (p[1] == '/' || p[1] == '\0'))) {
      return false;
    }
    if ((p == path + root_len || p[-1] == '/') &&
        (strcmp(p, ".") == 0 || strncmp(p, "./", 2) == 0 || strcmp(p, "..") == 0 || strncmp(p, "../", 3) == 0)) {
      return false;
    }
  }
  return true;
}

static bool sync_stat_path_allowed(const char* path) {
  const size_t root_len = sizeof(SYNC_ROOT) - 1U;
  const size_t len = strlen(path);

  if (len < root_len || len >= CONFIG_WEBOS_ADB_SYNC_PATH_MAX || strncmp(path, SYNC_ROOT, root_len) != 0) {
    return false;
  }
  for (const char* p = path + root_len; *p != '\0'; ++p) {
    if ((unsigned char)*p < 0x20U || *p == '\\' || *p == ':' || (*p == '/' && p[1] == '/')) {
      return false;
    }
    if ((p == path + root_len || p[-1] == '/') &&
        (strcmp(p, ".") == 0 || strncmp(p, "./", 2) == 0 || strcmp(p, "..") == 0 || strncmp(p, "../", 3) == 0)) {
      return false;
    }
  }
  return true;
}

static int send_stat(struct sync_context* context) {
  struct fs_dirent entry;
  uint8_t response[16];
  uint32_t mode = 0U;
  uint32_t size = 0U;

  context->spec[context->spec_used] = '\0';
  if (memchr(context->spec, '\0', context->spec_used) == NULL && sync_stat_path_allowed(context->spec) &&
      fs_stat(context->spec, &entry) == 0) {
    mode = entry.type == FS_DIR_ENTRY_DIR ? 0040755U : 0100644U;
    size = (uint32_t)MIN(entry.size, (size_t)UINT32_MAX);
  }

  sys_put_le32(SYNC_ID_STAT, response);
  sys_put_le32(mode, response + 4);
  sys_put_le32(size, response + 8);
  sys_put_le32(0U, response + 12); /* Zephyr fs_dirent has no portable mtime. */
  context->state = SYNC_REQUEST_HEADER;
  return webos_adb_stream_write(context->stream, response, sizeof(response));
}

static void cleanup_partial(struct sync_context* context) {
  if (context->file_open) {
    (void)fs_close(&context->file);
    context->file_open = false;
  }
  if (context->temporary_live) {
    (void)fs_unlink(context->temporary);
    context->temporary_live = false;
  }
}

static int send_status(struct sync_context* context, uint32_t id, const char* message) {
  uint8_t response[SYNC_FAIL_CAPACITY];
  size_t message_len = message == NULL ? 0U : strlen(message);

  message_len = MIN(message_len, sizeof(response) - 8U);
  sys_put_le32(id, response);
  sys_put_le32((uint32_t)message_len, response + 4);
  if (message_len != 0U) {
    memcpy(response + 8, message, message_len);
  }
  return webos_adb_stream_write(context->stream, response, 8U + message_len);
}

static int fail_transfer(struct sync_context* context, const char* reason, enum sync_state drain_state) {
  int ret;

  cleanup_partial(context);
  ret = send_status(context, SYNC_ID_FAIL, reason);
  if (ret != 0) {
    return ret;
  }
  context->state = drain_state;
  return 0;
}

static uint32_t side_nonce;

static int choose_side_paths(struct sync_context* context) {
  struct fs_dirent entry;

  for (size_t attempt = 0; attempt < 32U; ++attempt) {
    uint32_t token = ++side_nonce;
    int ret;

    ret = snprintk(context->temporary, sizeof(context->temporary), "%s.adb-part-%08x", context->path, token);
    if (ret < 0 || (size_t)ret >= sizeof(context->temporary)) {
      return -ENAMETOOLONG;
    }
    ret = snprintk(context->backup, sizeof(context->backup), "%s.adb-old-%08x", context->path, token);
    if (ret < 0 || (size_t)ret >= sizeof(context->backup)) {
      return -ENAMETOOLONG;
    }
    if (!path_exists(context->temporary, &entry) && !path_exists(context->backup, &entry)) {
      return 0;
    }
  }
  return -EEXIST;
}

static int start_send(struct sync_context* context) {
  char* comma;
  char* end;
  unsigned long mode;
  size_t path_len;
  struct fs_dirent entry;
  int ret;

  context->spec[context->spec_used] = '\0';
  if (memchr(context->spec, '\0', context->spec_used) != NULL) {
    return fail_transfer(context, "NUL in SEND path", SYNC_DRAIN_HEADER);
  }

  comma = strrchr(context->spec, ',');
  if (comma == NULL || comma == context->spec || comma[1] == '\0') {
    return fail_transfer(context, "SEND requires path,mode", SYNC_DRAIN_HEADER);
  }
  *comma = '\0';
  errno = 0;
  mode = strtoul(comma + 1, &end, 0);
  if (errno != 0 || *end != '\0' || mode > UINT32_MAX) {
    return fail_transfer(context, "invalid SEND mode", SYNC_DRAIN_HEADER);
  }
  /* Zephyr fs has no portable chmod or symlink creation; accept regular files only. */
  if ((mode & 0170000UL) != 0UL && (mode & 0170000UL) != 0100000UL) {
    return fail_transfer(context, "only regular files supported", SYNC_DRAIN_HEADER);
  }

  path_len = strlen(context->spec);
  if (!sync_path_allowed(context->spec) || path_len >= sizeof(context->path)) {
    return fail_transfer(context, "path outside /STORAGE:", SYNC_DRAIN_HEADER);
  }
  memcpy(context->path, context->spec, path_len + 1U);

  ret = choose_side_paths(context);
  if (ret != 0) {
    return fail_transfer(context, ret == -ENAMETOOLONG ? "path too long" : "no free staging name", SYNC_DRAIN_HEADER);
  }
  if (path_exists(context->path, &entry) && entry.type != FS_DIR_ENTRY_FILE) {
    return fail_transfer(context, "destination is not a file", SYNC_DRAIN_HEADER);
  }

  fs_file_t_init(&context->file);
  ret = fs_open(&context->file, context->temporary, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
  if (ret != 0) {
    return fail_transfer(context, "cannot create partial file", SYNC_DRAIN_HEADER);
  }
  context->file_open = true;
  context->temporary_live = true;
  context->state = SYNC_SEND_HEADER;
  return 0;
}

static int replace_destination(struct sync_context* context) {
  struct fs_dirent entry;
  bool had_destination;
  int ret;

  ret = fs_close(&context->file);
  context->file_open = false;
  if (ret != 0) {
    return ret;
  }

  had_destination = path_exists(context->path, &entry);
  if (had_destination) {
    ret = fs_rename(context->path, context->backup);
    if (ret != 0) {
      return ret;
    }
  }

  ret = fs_rename(context->temporary, context->path);
  if (ret != 0) {
    if (had_destination) {
      (void)fs_rename(context->backup, context->path);
    }
    return ret;
  }
  context->temporary_live = false;
  if (had_destination) {
    (void)fs_unlink(context->backup);
  }
  return 0;
}

static int finish_send(struct sync_context* context, uint32_t timestamp) {
  int ret;

  ARG_UNUSED(timestamp); /* Zephyr fs has no portable path timestamp setter. */
  ret = replace_destination(context);
  if (ret != 0) {
    return fail_transfer(context, "cannot install completed file", SYNC_DRAIN_HEADER);
  }
  ret = send_status(context, SYNC_ID_OKAY, NULL);
  if (ret == 0) {
    context->state = SYNC_REQUEST_HEADER;
  }
  return ret;
}

static int process_header(struct sync_context* context) {
  uint32_t id = sys_get_le32(context->header);
  uint32_t value = sys_get_le32(context->header + 4);

  context->header_used = 0U;
  if (context->state == SYNC_REQUEST_HEADER) {
    if (id == SYNC_ID_QUIT && value == 0U) {
      webos_adb_stream_close(context->stream);
      return 0;
    }
    if ((id == SYNC_ID_SEND || id == SYNC_ID_STAT) && value != 0U && value < sizeof(context->spec)) {
      context->remaining = value;
      context->spec_used = 0U;
      context->state = id == SYNC_ID_SEND ? SYNC_REQUEST_SPEC : SYNC_STAT_PATH;
      return 0;
    }
    if (send_status(context, SYNC_ID_FAIL, "only bounded STAT and SEND are supported") != 0) {
      return -EIO;
    }
    context->state = SYNC_DISCARD_AND_CLOSE;
    context->remaining = 0U;
    webos_adb_stream_close(context->stream);
    return 0;
  }

  if (context->state == SYNC_DRAIN_HEADER) {
    if (id == SYNC_ID_DONE) {
      context->state = SYNC_REQUEST_HEADER;
      return 0;
    }
    if (id != SYNC_ID_DATA || value > CONFIG_WEBOS_ADB_SYNC_DATA_MAX) {
      webos_adb_stream_close(context->stream);
      return 0;
    }
    context->remaining = value;
    context->state = value == 0U ? SYNC_DRAIN_HEADER : SYNC_DRAIN_DATA;
    return 0;
  }

  if (id == SYNC_ID_DONE) {
    return finish_send(context, value);
  }
  if (id != SYNC_ID_DATA || value > CONFIG_WEBOS_ADB_SYNC_DATA_MAX) {
    int ret = fail_transfer(context, "invalid or oversized DATA", SYNC_DISCARD_AND_CLOSE);

    webos_adb_stream_close(context->stream);
    return ret;
  }
  context->remaining = value;
  context->state = value == 0U ? SYNC_SEND_HEADER : SYNC_SEND_DATA;
  return 0;
}

static int sync_open(struct webos_adb_stream* stream, const uint8_t* suffix, size_t suffix_len) {
  struct sync_context* context = NULL;

  if (suffix_len != 0U) {
    return -EINVAL; /* Only the exact legacy service name "sync:" is valid. */
  }
  ARG_UNUSED(suffix);
  for (size_t i = 0; i < ARRAY_SIZE(contexts); ++i) {
    if (contexts[i].stream == NULL) {
      context = &contexts[i];
      break;
    }
  }
  if (context == NULL) {
    return -ENOSPC;
  }
  memset(context, 0, sizeof(*context));
  context->stream = stream;
  context->state = SYNC_REQUEST_HEADER;
  return 0;
}

static int sync_write(struct webos_adb_stream* stream, const uint8_t* data, size_t len) {
  struct sync_context* context = find_context(stream);

  if (context == NULL || (data == NULL && len != 0U)) {
    return -EINVAL;
  }

  while (len != 0U) {
    if (context->state == SYNC_DISCARD_AND_CLOSE && context->remaining == 0U) {
      return 0;
    }
    if (context->state == SYNC_REQUEST_HEADER || context->state == SYNC_SEND_HEADER ||
        context->state == SYNC_DRAIN_HEADER) {
      size_t count = MIN(len, sizeof(context->header) - context->header_used);

      memcpy(context->header + context->header_used, data, count);
      context->header_used += count;
      data += count;
      len -= count;
      if (context->header_used == sizeof(context->header)) {
        int ret = process_header(context);
        if (ret != 0) {
          return ret;
        }
      }
      continue;
    }

    if (context->state == SYNC_REQUEST_SPEC || context->state == SYNC_STAT_PATH) {
      enum sync_state request_state = context->state;
      size_t count = MIN(len, (size_t)context->remaining);

      memcpy(context->spec + context->spec_used, data, count);
      context->spec_used += count;
      context->remaining -= (uint32_t)count;
      data += count;
      len -= count;
      if (context->remaining == 0U) {
        int ret = request_state == SYNC_STAT_PATH ? send_stat(context) : start_send(context);

        if (ret != 0) {
          return ret;
        }
      }
      continue;
    }

    if (context->state == SYNC_SEND_DATA) {
      size_t count = MIN(len, (size_t)context->remaining);
      ssize_t written = fs_write(&context->file, data, count);

      if (written < 0 || (size_t)written != count) {
        /* The rest of this DATA record is consumed without retaining it. */
        context->remaining -= (uint32_t)count;
        data += count;
        len -= count;
        if (fail_transfer(context, "write failed", context->remaining == 0U ? SYNC_DRAIN_HEADER : SYNC_DRAIN_DATA) !=
            0) {
          return -EIO;
        }
        continue;
      }
      context->remaining -= (uint32_t)count;
      data += count;
      len -= count;
      if (context->remaining == 0U) {
        context->state = SYNC_SEND_HEADER;
      }
      continue;
    }

    if (context->state == SYNC_DRAIN_DATA || context->state == SYNC_DISCARD_AND_CLOSE) {
      size_t count = MIN(len, (size_t)context->remaining);

      context->remaining -= (uint32_t)count;
      data += count;
      len -= count;
      if (context->remaining == 0U) {
        if (context->state == SYNC_DISCARD_AND_CLOSE) {
          webos_adb_stream_close(context->stream);
        } else {
          context->state = SYNC_DRAIN_HEADER;
        }
      }
    }
  }
  return 0;
}

static void sync_tx_ready(struct webos_adb_stream* stream) {
  ARG_UNUSED(stream); /* Push responses fit in one bounded service WRTE. */
}

static void sync_close(struct webos_adb_stream* stream) {
  struct sync_context* context = find_context(stream);

  if (context != NULL) {
    cleanup_partial(context);
    memset(context, 0, sizeof(*context));
  }
}

const struct webos_adb_service_ops webos_adb_sync_service_ops = {
    .open = sync_open,
    .write = sync_write,
    .tx_ready = sync_tx_ready,
    .close = sync_close,
};
