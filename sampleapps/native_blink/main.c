extern void sleep_ms(int ms);
extern void log_print(const char* msg);
extern int dev_fs_write(const char* path, const char* data);
extern int dev_fs_read(const char* path, char* buf, int buf_len);

static void itos(char* buf, int n) {
  char tmp[12];
  int i = 0;

  if (n == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }

  while (n > 0) {
    tmp[i++] = '0' + (n % 10);
    n /= 10;
  }

  for (int j = 0; j < i; j++) {
    buf[j] = tmp[i - 1 - j];
  }
  buf[i] = '\0';
}

static void make_path(char* buf, int sz, const char* prefix, int pin, const char* suffix) {
  const char* p;
  int i = 0;

  for (p = prefix; *p && i < sz - 1; p++, i++) {
    buf[i] = *p;
  }

  itos(buf + i, pin);
  while (buf[i]) {
    i++;
  }

  for (p = suffix; *p && i < sz - 1; p++, i++) {
    buf[i] = *p;
  }
  buf[i] = '\0';
}

int main(int argc, char** argv) {
  int pin = 2;
  int count = 10;
  char path[64];
  char buf[16];
  int i;

  if (argc > 1) {
    pin = 0;
    for (const char* p = argv[1]; *p >= '0' && *p <= '9'; p++) {
      pin = pin * 10 + (*p - '0');
    }
  }

  log_print("blink: starting");

  make_path(path, sizeof(path), "/dev/gpio/", pin, "/direction");
  dev_fs_write(path, "out");

  for (i = 0; i < count; i++) {
    make_path(path, sizeof(path), "/dev/gpio/", pin, "/value");
    dev_fs_write(path, "1");
    log_print("ON");

    sleep_ms(500);

    dev_fs_write(path, "0");
    log_print("OFF");

    if (i < count - 1) {
      sleep_ms(500);
    }
  }

  make_path(path, sizeof(path), "/dev/gpio/", pin, "/value");
  dev_fs_read(path, buf, sizeof(buf));
  log_print(buf);

  log_print("blink: done");
  return 0;
}
