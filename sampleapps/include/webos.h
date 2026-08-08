#ifndef WEBOS_SAMPLE_SDK_H
#define WEBOS_SAMPLE_SDK_H
#include "../../sdk/c/webos.h"
/* Source compatibility for applications written before ABI v1. */
void log_print(const char* message);
void sleep_ms(unsigned int milliseconds);
int dev_fs_write(const char* path, const void* data, unsigned int length);
int dev_fs_read(const char* path, void* data, unsigned int capacity);
#endif
