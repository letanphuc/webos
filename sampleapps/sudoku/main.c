#include "webos.h"

#define DEFAULT_URL "https://api.api-ninjas.com/v1/sudokugenerate?difficulty=easy"

static unsigned int text_len(const char* text) {
  unsigned int length = 0;

  while (text[length] != '\0') {
    length++;
  }
  return length;
}

static int append_text(char* output, unsigned int capacity, unsigned int* used, const char* text) {
  unsigned int length = text_len(text);

  if (length > capacity - *used) {
    return -1;
  }
  for (unsigned int i = 0; i < length; i++) {
    output[*used + i] = text[i];
  }
  *used += length;
  return 0;
}

static void append_number(char* output, unsigned int capacity, unsigned int* used, unsigned int value) {
  char digits[10];
  unsigned int count = 0;

  do {
    digits[count++] = (char)('0' + value % 10);
    value /= 10;
  } while (value > 0 && count < sizeof(digits));

  while (count > 0 && *used < capacity) {
    output[(*used)++] = digits[--count];
  }
}

int main(int argc, char** argv) {
  const char* url = DEFAULT_URL;
  char headers[256];
  unsigned char body[2048];
  char status_message[64];
  unsigned int headers_len = 0;
  unsigned int status_len = 0;
  struct web_http_response response = {
      .struct_size = sizeof(response),
  };
  int ret;

  if (argc < 2) {
    log_print("usage: sudoku <api-key> [url]");
    return 1;
  }
  if (argc > 2) {
    url = argv[2];
  }

  if (append_text(headers, sizeof(headers), &headers_len, "X-Api-Key: ") != 0 ||
      append_text(headers, sizeof(headers), &headers_len, argv[1]) != 0 ||
      append_text(headers, sizeof(headers), &headers_len, "\r\nAccept: application/json\r\n") != 0) {
    log_print("sudoku: API key is too long");
    return 1;
  }

  log_print("sudoku: requesting puzzle");
  ret = web_http_request(WEB_HTTP_GET, url, text_len(url), headers, headers_len, 0, 0, body, sizeof(body) - 1,
                         &response, sizeof(response), 15000);
  if (ret != WEB_HTTP_OK) {
    status_len = 0;
    append_text(status_message, sizeof(status_message) - 1, &status_len, "sudoku: request error ");
    if (ret < 0) {
      append_text(status_message, sizeof(status_message) - 1, &status_len, "-");
      append_number(status_message, sizeof(status_message) - 1, &status_len, (unsigned int)-ret);
    } else {
      append_number(status_message, sizeof(status_message) - 1, &status_len, (unsigned int)ret);
    }
    status_message[status_len] = '\0';
    log_print(status_message);
    return 1;
  }

  status_len = 0;
  append_text(status_message, sizeof(status_message) - 1, &status_len, "sudoku: HTTP ");
  append_number(status_message, sizeof(status_message) - 1, &status_len, response.status_code);
  status_message[status_len] = '\0';
  log_print(status_message);

  body[response.body_len] = '\0';
  log_print((const char*)body);
  return response.status_code >= 200 && response.status_code < 300 ? 0 : 1;
}
