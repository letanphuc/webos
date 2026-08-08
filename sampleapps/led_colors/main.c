#include "webos.h"

static const char* color_names[] = {
    "000", "001", "010", "011", "100", "101", "110", "111",
};

int main(int argc, char** argv) {
  const char* path = "/dev/led/48/color";
  int rounds = 3;
  int delay_ms = 500;

  if (argc > 1) {
    rounds = 0;
    for (const char* p = argv[1]; *p >= '0' && *p <= '9'; p++) {
      rounds = rounds * 10 + (*p - '0');
    }
  }

  log_print("led_colors: starting");

  for (int round = 0; round < rounds; round++) {
    for (int value = 0; value < 8; value++) {
      unsigned char rgb[3] = {
          (value & 4) ? 0xff : 0x00,
          (value & 2) ? 0xff : 0x00,
          (value & 1) ? 0xff : 0x00,
      };

      log_print(color_names[value]);
      dev_fs_write(path, rgb, sizeof(rgb));
      sleep_ms(delay_ms);
    }
  }

  {
    unsigned char off = 0;
    dev_fs_write(path, &off, sizeof(off));
  }

  log_print("led_colors: done");
  return 0;
}
