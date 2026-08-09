/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <webos/shell_exec.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/util.h>

static K_MUTEX_DEFINE(shell_lock);

static void copy_output(const struct shell* sh, char* output, size_t capacity, size_t* output_len, bool* truncated) {
  const char* dummy_output;
  size_t dummy_len;
  size_t copy_len;

  dummy_output = shell_backend_dummy_get_output(sh, &dummy_len);
  copy_len = MIN(dummy_len, capacity - 1U);
  memcpy(output, dummy_output, copy_len);
  output[copy_len] = '\0';
  *output_len = copy_len;
  if (truncated != NULL) {
    *truncated = copy_len != dummy_len;
  }
}

int webos_shell_execute(const char* command, char* output, size_t capacity, size_t* output_len, bool* truncated) {
  const struct shell* sh;
  int ret;

  if (command == NULL || output == NULL || capacity == 0U || output_len == NULL) {
    return -EINVAL;
  }

  ret = k_mutex_lock(&shell_lock, K_MSEC(CONFIG_WEBOS_SHELL_EXEC_LOCK_TIMEOUT_MS));
  if (ret != 0) {
    return ret;
  }

  sh = shell_backend_dummy_get_ptr();
  shell_backend_dummy_clear_output(sh);
  ret = shell_execute_cmd(sh, command);
  copy_output(sh, output, capacity, output_len, truncated);
  k_mutex_unlock(&shell_lock);
  return ret;
}

int webos_shell_push_input(const char* input, size_t input_len, char* output, size_t capacity, size_t* output_len,
                           bool* truncated) {
  const struct shell* sh;
  int ret;

  if ((input == NULL && input_len != 0U) || output == NULL || capacity == 0U || output_len == NULL) {
    return -EINVAL;
  }

  ret = k_mutex_lock(&shell_lock, K_MSEC(CONFIG_WEBOS_SHELL_EXEC_LOCK_TIMEOUT_MS));
  if (ret != 0) {
    return ret;
  }

  sh = shell_backend_dummy_get_ptr();
  ret = shell_backend_dummy_push_input(sh, input, input_len);
  if (ret == 0) {
    k_sleep(K_MSEC(20));
  }
  copy_output(sh, output, capacity, output_len, truncated);
  k_mutex_unlock(&shell_lock);
  return ret;
}

void webos_shell_clear_output(void) {
  const struct shell* sh;

  if (k_mutex_lock(&shell_lock, K_MSEC(CONFIG_WEBOS_SHELL_EXEC_LOCK_TIMEOUT_MS)) != 0) {
    return;
  }
  sh = shell_backend_dummy_get_ptr();
  shell_backend_dummy_clear_output(sh);
  k_mutex_unlock(&shell_lock);
}
