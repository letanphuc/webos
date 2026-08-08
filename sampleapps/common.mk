WASI_SDK ?= $(HOME)/.local/share/wasi-sdk
CC := $(WASI_SDK)/bin/clang
OBJCOPY := $(WASI_SDK)/bin/llvm-objcopy
ABI_METADATA := ../../sdk/webos-abi-v1.bin
SRC ?= main.c

CFLAGS ?= -O3 --target=wasm32-wasip1 -fvisibility=default
CFLAGS += -I../include
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

$(TARGET): $(SRC) ../include/webos.h $(ABI_METADATA)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(OBJCOPY) --add-section .custom_section.webos.abi=$(ABI_METADATA) $@

clean:
	rm -f $(TARGET)
