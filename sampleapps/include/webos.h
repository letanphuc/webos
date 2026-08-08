#ifndef WEBOS_H_
#define WEBOS_H_

#ifdef __cplusplus
extern "C" {
#endif

int gpio_set(unsigned int pin, unsigned int value);
int gpio_get(unsigned int pin);
void sleep_ms(unsigned int ms);
void log_print(const char* message);
int dev_fs_write(const char* path, const void* data, unsigned int length);
int dev_fs_read(const char* path, void* data, unsigned int capacity);

#ifdef __cplusplus
}
#endif

#endif /* WEBOS_H_ */
