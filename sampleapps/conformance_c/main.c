#include "webos.h"
WEBOS_DECLARE_ABI_VERSION()
int main(void) {
  static const char message[] = "WebOS ABI v1 C conformance";
  char byte;
  webos_log(WEBOS_LOG_INFO, message, sizeof(message) - 1);
  webos_ready();
  (void)webos_write("/STORAGE:/apps/conformance-c/data/probe", message, sizeof(message) - 1);
  (void)webos_read("/STORAGE:/apps/conformance-c/data/probe", &byte, sizeof(byte));
  webos_heartbeat();
  webos_sleep_ms(1);
  return 0;
}
