#include "services/iwasm/iwasm.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

#include "lib_export.h"
#include "services/http_client/http_client.h"
#include "wasm_export.h"

LOG_MODULE_REGISTER(iwasm, LOG_LEVEL_INF);

static bool runtime_ready;
static const struct shell* active_shell;
static K_MUTEX_DEFINE(exec_lock);

int webos_iwasm_vprintf(const char* format, va_list ap) {
  if (active_shell) {
    shell_vfprintf(active_shell, SHELL_NORMAL, format, ap);
    return 0;
  }

  vprintk(format, ap);
  return 0;
}

struct mem_header {
  void* raw;
  size_t size;
};

static void* iwasm_malloc(unsigned int size) {
  size_t header_sz = sizeof(struct mem_header);
  size_t total = header_sz + size;
  uint8_t* raw = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 8, total);

  if (!raw) {
    return NULL;
  }

  uintptr_t user = (uintptr_t)(raw + header_sz);
  struct mem_header* hdr = (struct mem_header*)(user - header_sz);

  hdr->raw = raw;
  hdr->size = size;
  return (void*)user;
}

static void iwasm_free(void* ptr) {
  if (!ptr) {
    return;
  }
  uintptr_t user = (uintptr_t)ptr;
  struct mem_header* hdr = (struct mem_header*)(user - sizeof(struct mem_header));

  shared_multi_heap_free(hdr->raw);
}

static void* iwasm_realloc(void* ptr, unsigned int size) {
  uintptr_t user;
  struct mem_header* hdr;
  size_t old_size;
  void* new_ptr;

  if (!ptr) {
    return iwasm_malloc(size);
  }
  if (size == 0) {
    iwasm_free(ptr);
    return NULL;
  }

  user = (uintptr_t)ptr;
  hdr = (struct mem_header*)(user - sizeof(struct mem_header));
  old_size = hdr->size;

  new_ptr = iwasm_malloc(size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, old_size < (size_t)size ? old_size : (size_t)size);
    iwasm_free(ptr);
  }
  return new_ptr;
}

static void sleep_ms(wasm_exec_env_t exec_env, uint32_t ms) { k_sleep(K_MSEC(ms)); }

static void log_print(wasm_exec_env_t exec_env, const char* msg) {
  if (msg) {
    LOG_INF("payload: %s", msg);
  }
}

static int32_t dev_fs_write(wasm_exec_env_t exec_env, const char* path, const uint8_t* data, uint32_t len) {
  wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
  struct fs_file_t file;
  fs_mode_t flags = FS_O_CREATE | FS_O_WRITE;
  int ret;

  if (!path || (!data && len > 0)) {
    return -EINVAL;
  }
  if (len > 0 && !wasm_runtime_validate_native_addr(module_inst, (void*)data, len)) {
    return -EFAULT;
  }

  if (strncmp(path, "/dev/", strlen("/dev/")) != 0) {
    flags |= FS_O_TRUNC;
  }

  fs_file_t_init(&file);
  ret = fs_open(&file, path, flags);
  if (ret != 0) {
    LOG_ERR("payload: dev_fs_write open(%s) err %d", path, ret);
    return ret;
  }

  ret = (int)fs_write(&file, data, len);
  fs_close(&file);

  if (ret < 0) {
    LOG_ERR("payload: dev_fs_write write(%s) err %d", path, ret);
  }
  return ret;
}

static int32_t dev_fs_read(wasm_exec_env_t exec_env, const char* path, uint8_t* data, uint32_t capacity) {
  wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
  struct fs_file_t file;
  int ret;

  if (!path || (!data && capacity > 0)) {
    return -EINVAL;
  }
  if (capacity > 0 && !wasm_runtime_validate_native_addr(module_inst, data, capacity)) {
    return -EFAULT;
  }

  fs_file_t_init(&file);
  ret = fs_open(&file, path, FS_O_READ);
  if (ret != 0) {
    LOG_ERR("payload: dev_fs_read open(%s) err %d", path, ret);
    return ret;
  }

  ret = (int)fs_read(&file, data, capacity);
  fs_close(&file);

  if (ret < 0) {
    LOG_ERR("payload: dev_fs_read read(%s) err %d", path, ret);
  }
  return ret;
}

static int32_t web_http_request(wasm_exec_env_t exec_env, uint32_t method, const uint8_t* url, uint32_t url_len,
                                const uint8_t* headers, uint32_t headers_len, const uint8_t* request_body,
                                uint32_t request_body_len, uint8_t* response_body, uint32_t response_capacity,
                                struct web_http_response* response, uint32_t response_size, uint32_t timeout_ms) {
  wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
  char* url_copy = NULL;
  char* headers_copy = NULL;
  int32_t ret;

  if (!url || url_len == 0 || url_len > CONFIG_WEBOS_WASM_HTTP_MAX_URL_LEN ||
      headers_len > CONFIG_WEBOS_WASM_HTTP_MAX_HEADERS_LEN ||
      request_body_len > CONFIG_WEBOS_WASM_HTTP_MAX_REQUEST_BODY ||
      response_capacity > CONFIG_WEBOS_WASM_HTTP_MAX_RESPONSE_BODY || response_size < sizeof(*response) ||
      timeout_ms > CONFIG_WEBOS_WASM_HTTP_MAX_TIMEOUT_MS) {
    return WEB_HTTP_ERR_INVALID;
  }

  if (!wasm_runtime_validate_native_addr(module_inst, (void*)url, url_len) ||
      (headers_len > 0 && (!headers || !wasm_runtime_validate_native_addr(module_inst, (void*)headers, headers_len))) ||
      (request_body_len > 0 &&
       (!request_body || !wasm_runtime_validate_native_addr(module_inst, (void*)request_body, request_body_len))) ||
      (response_capacity > 0 &&
       (!response_body || !wasm_runtime_validate_native_addr(module_inst, response_body, response_capacity))) ||
      !response || !wasm_runtime_validate_native_addr(module_inst, response, sizeof(*response))) {
    return WEB_HTTP_ERR_INVALID;
  }

  if (memchr(url, '\0', url_len) != NULL || (headers_len > 0 && memchr(headers, '\0', headers_len) != NULL)) {
    return WEB_HTTP_ERR_INVALID;
  }

  if (response->struct_size < sizeof(*response)) {
    return WEB_HTTP_ERR_INVALID;
  }

  url_copy = k_malloc(url_len + 1);
  if (!url_copy) {
    return WEB_HTTP_ERR_BUSY;
  }
  memcpy(url_copy, url, url_len);
  url_copy[url_len] = '\0';

  if (headers_len > 0) {
    headers_copy = k_malloc(headers_len + 1);
    if (!headers_copy) {
      k_free(url_copy);
      return WEB_HTTP_ERR_BUSY;
    }
    memcpy(headers_copy, headers, headers_len);
    headers_copy[headers_len] = '\0';
  }

  ret = webos_http_request(method, url_copy, headers_copy, request_body, request_body_len, response_body,
                           response_capacity, response, timeout_ms);
  k_free(headers_copy);
  k_free(url_copy);
  return ret;
}

static NativeSymbol native_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(sleep_ms, "(i)"),
    EXPORT_WASM_API_WITH_SIG(log_print, "($)"),
    EXPORT_WASM_API_WITH_SIG(dev_fs_write, "($*~)i"),
    EXPORT_WASM_API_WITH_SIG(dev_fs_read, "($*~)i"),
    EXPORT_WASM_API_WITH_SIG(web_http_request, "(i*~*~*~*~*~i)i"),
};

int iwasm_init(void) {
  RuntimeInitArgs init_args;

  memset(&init_args, 0, sizeof(init_args));
  init_args.mem_alloc_type = Alloc_With_Allocator;
  init_args.mem_alloc_option.allocator.malloc_func = iwasm_malloc;
  init_args.mem_alloc_option.allocator.realloc_func = iwasm_realloc;
  init_args.mem_alloc_option.allocator.free_func = iwasm_free;
  init_args.native_module_name = "env";
  init_args.native_symbols = native_symbols;
  init_args.n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);

  if (!wasm_runtime_full_init(&init_args)) {
    LOG_ERR("Failed to initialize WAMR runtime");
    return -EIO;
  }

  runtime_ready = true;
  LOG_INF("iwasm runtime initialized");
  return 0;
}

static int iwasm_exec_file(const struct shell* sh, const char* path, int app_argc, char** app_argv) {
  struct fs_file_t file;
  ssize_t file_size;
  uint8_t* buf = NULL;
  wasm_module_t module = NULL;
  wasm_module_inst_t module_inst = NULL;
  char error_buf[128];
  int ret = -1;

  if (!runtime_ready) {
    LOG_ERR("Runtime not initialized");
    return -EIO;
  }

  fs_file_t_init(&file);
  ret = fs_open(&file, path, FS_O_READ);
  if (ret != 0) {
    LOG_ERR("Cannot open %s (err %d)", path, ret);
    return ret;
  }

  ret = fs_seek(&file, 0, FS_SEEK_END);
  if (ret != 0) {
    LOG_ERR("Cannot seek %s (err %d)", path, ret);
    fs_close(&file);
    return ret;
  }

  file_size = fs_tell(&file);
  if (file_size <= 0) {
    LOG_ERR("File %s is empty or unreadable", path);
    fs_close(&file);
    return -EINVAL;
  }

  fs_seek(&file, 0, FS_SEEK_SET);

  buf = (uint8_t*)k_malloc(file_size);
  if (!buf) {
    LOG_ERR("Out of memory reading %s (%d bytes)", path, file_size);
    fs_close(&file);
    return -ENOMEM;
  }

  ret = fs_read(&file, buf, file_size);
  fs_close(&file);
  if (ret < 0) {
    LOG_ERR("Failed to read %s (err %d)", path, ret);
    goto cleanup_buf;
  }

  error_buf[0] = '\0';
  module = wasm_runtime_load(buf, (uint32_t)file_size, error_buf, sizeof(error_buf));
  if (!module) {
    LOG_ERR("Load failed: %s", error_buf);
    ret = -EINVAL;
    goto cleanup_buf;
  }

  LOG_INF("iwasm: instantiating module...");
  module_inst = wasm_runtime_instantiate(module, 65536, 65536, error_buf, sizeof(error_buf));
  LOG_INF("iwasm: instantiate done, inst=%p", (void*)module_inst);
  if (!module_inst) {
    LOG_ERR("Instantiate failed: %s", error_buf);
    ret = -EINVAL;
    goto cleanup_module;
  }

  active_shell = sh;
  if (!wasm_application_execute_main(module_inst, app_argc, app_argv)) {
    const char* exc = wasm_runtime_get_exception(module_inst);

    active_shell = NULL;
    LOG_ERR("Execute failed: %s", exc ? exc : "unknown");
    ret = -EIO;
  } else {
    active_shell = NULL;
    LOG_INF("Executed %s successfully", path);
    ret = 0;
  }

  wasm_runtime_deinstantiate(module_inst);
cleanup_module:
  wasm_runtime_unload(module);
cleanup_buf:
  k_free(buf);
  return ret;
}

static int cmd_iwasm_exec(const struct shell* sh, size_t argc, char** argv) {
  if (argc < 2) {
    shell_error(sh, "Usage: iwasm exec <file> [args...]");
    return -EINVAL;
  }

  const char* file = argv[1];
  int app_argc = (int)argc - 1;
  char** app_argv = &argv[1];

  k_mutex_lock(&exec_lock, K_FOREVER);
  int ret = iwasm_exec_file(sh, file, app_argc, app_argv);
  k_mutex_unlock(&exec_lock);

  if (ret != 0) {
    shell_error(sh, "iwasm exec failed: %d", ret);
  }
  return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(iwasm_subcmds,
                               SHELL_CMD(exec, NULL, "Execute a WASM/AOT file: iwasm exec <file> [args...]",
                                         cmd_iwasm_exec),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(iwasm, &iwasm_subcmds, "iwasm runtime commands", NULL);
