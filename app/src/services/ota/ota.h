#ifndef WEBOS_SERVICES_OTA_H
#define WEBOS_SERVICES_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEBOS_OTA_REBOOT_DELAY_MS 2000

void ota_init(void);
int ota_begin(void);
int ota_write(const uint8_t* data, size_t len, bool final);
int ota_finish(size_t* written);
void ota_abort(void);

#endif
