#include "services/ssh/ssh.h"

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/ssh/keygen.h>
#include <zephyr/net/ssh/server.h>
#include <zephyr/shell/shell_ssh.h>

#include "services/fs/fs.h"

LOG_MODULE_REGISTER(webos_ssh, LOG_LEVEL_INF);

static int read_file_bin(const char* path, uint8_t* buf, size_t buf_len, size_t* out_len) {
  struct fs_file_t file;
  int ret;

  fs_file_t_init(&file);
  ret = fs_open(&file, path, FS_O_READ);
  if (ret != 0) {
    return ret;
  }

  ret = fs_read(&file, buf, buf_len);
  if (ret >= 0) {
    *out_len = ret;
  }

  int close_ret = fs_close(&file);

  return ret < 0 ? ret : close_ret;
}

void ssh_service_start(void) {
  static const char ssh_key_dir[] = WEBOS_MOUNT_POINT "/config/ssh";
  static const char ssh_host_key_path[] = WEBOS_MOUNT_POINT "/config/ssh/ssh_host_rsa.der";
  static uint8_t key_buf[2048];
  struct net_sockaddr_storage bind_addr;
  struct ssh_server* sshd;
  size_t key_len = 0;
  int ret;

  LOG_INF("Loading SSH host key from %s", ssh_host_key_path);
  ret = read_file_bin(ssh_host_key_path, key_buf, sizeof(key_buf), &key_len);
  if (ret == -ENOENT) {
    LOG_INF("SSH host key not found at %s; push keys to %s first", ssh_host_key_path, ssh_key_dir);
    return;
  }
  if (ret != 0) {
    LOG_ERR("SSH host key read failed: %d", ret);
    return;
  }
  if (key_len == 0) {
    LOG_ERR("SSH host key is empty: %s", ssh_host_key_path);
    return;
  }

  ret = ssh_keygen_import(0, true, SSH_HOST_KEY_FORMAT_DER, key_buf, key_len);
  if (ret != 0) {
    LOG_ERR("SSH host key import failed: %d", ret);
    return;
  }

  memset(&bind_addr, 0, sizeof(bind_addr));
  net_sin(net_sad(&bind_addr))->sin_family = AF_INET;
  net_sin(net_sad(&bind_addr))->sin_port = htons(CONFIG_SSH_PORT);
  net_sin(net_sad(&bind_addr))->sin_addr.s_addr = INADDR_ANY;

  sshd = ssh_server_instance(0);
  if (sshd == NULL) {
    LOG_ERR("SSH server instance 0 unavailable");
    return;
  }

  ret = ssh_server_start(sshd, net_sad(&bind_addr), 0, CONFIG_WEBOS_SSH_USERNAME, CONFIG_WEBOS_SSH_PASSWORD, NULL, 0,
                         shell_sshd_event_callback, shell_sshd_transport_event_callback, NULL);
  if (ret != 0) {
    LOG_ERR("SSH server start failed: %d", ret);
    return;
  }

  LOG_INF("SSH server started with host key %s", ssh_host_key_path);
}
