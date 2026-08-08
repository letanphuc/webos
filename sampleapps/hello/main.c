#include "webos.h"

static char* append_uint(char* out, unsigned int value) {
  char digits[10];
  int count = 0;

  do {
    digits[count++] = (char)('0' + value % 10);
    value /= 10;
  } while (value > 0);

  while (count > 0) {
    *out++ = digits[--count];
  }
  return out;
}

int main(int argc, char** argv) {
  unsigned int a = 0;
  unsigned int b = 1;
  char output[64];
  char* cursor = output;

  for (int i = 0; i < 10; i++) {
    cursor = append_uint(cursor, a);
    *cursor++ = i == 9 ? '\0' : ' ';

    unsigned int next = a + b;
    a = b;
    b = next;
  }

  log_print(output);
  return 0;
}
