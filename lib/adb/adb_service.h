/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WEBOS_LIB_ADB_SERVICE_H_
#define WEBOS_LIB_ADB_SERVICE_H_

#include <stddef.h>
#include <stdint.h>

struct webos_adb_stream;

/* All payload pointers passed to service callbacks are borrowed until return. */
struct webos_adb_service_ops {
  int (*open)(struct webos_adb_stream* stream, const uint8_t* suffix, size_t suffix_len);
  int (*write)(struct webos_adb_stream* stream, const uint8_t* data, size_t len);
  void (*tx_ready)(struct webos_adb_stream* stream);
  void (*close)(struct webos_adb_stream* stream);
};

/* Copies data into the router-owned one-WRTE transmit slot. */
int webos_adb_stream_write(struct webos_adb_stream* stream, const uint8_t* data, size_t len);

/* Defers CLSE until any queued WRTE has been acknowledged by the peer. */
void webos_adb_stream_close(struct webos_adb_stream* stream);

size_t webos_adb_stream_max_payload(const struct webos_adb_stream* stream);

#if defined(CONFIG_WEBOS_ADB_SHELL)
extern const struct webos_adb_service_ops webos_adb_shell_service_ops;
#endif
#if defined(CONFIG_WEBOS_ADB_SYNC)
extern const struct webos_adb_service_ops webos_adb_sync_service_ops;
#endif

#endif /* WEBOS_LIB_ADB_SERVICE_H_ */
