#include <stdio.h>

int main(int argc, char** argv) {
  int a = 0;
  int b = 1;

  for (int i = 0; i < 10; i++) {
    printf("%d%s", a, i == 9 ? "\n" : " ");

    int next = a + b;
    a = b;
    b = next;
  }

  return 0;
}
