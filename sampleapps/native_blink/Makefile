WASI_SDK ?= /opt/wasi-sdk
CC := $(WASI_SDK)/bin/clang

TARGET := native_blink.wasm
SRC := main.c

CFLAGS := -O3 --target=wasm32-wasi -fvisibility=default -Wno-implicit-function-declaration
LDFLAGS := \
	-z stack-size=4096 \
	-Wl,--initial-memory=65536 \
	-Wl,--export=main \
	-Wl,--export=__main_argc_argv \
	-Wl,--export=__data_end \
	-Wl,--export=__heap_base \
	-Wl,--strip-all,--no-entry \
	-Wl,--allow-undefined \
	-nostdlib

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
