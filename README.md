# WebOS

WebOS is a Zephyr-based firmware project for ESP32-S3 devices. The goal is to keep a stable host OS on the device and run small, disposable application payloads through WebAssembly.

The current tree provides Wi-Fi management, persistent FatFS storage, devfs hardware access, OTA, health reporting, and a WAMR payload runtime on ESP32-S3. The firmware and sample workflow have been validated on physical hardware.

## Implemented Features

- ESP32-S3 firmware with MCUboot, 8 MB PSRAM, Wi-Fi, and a flash-backed FatFS volume at `/STORAGE:`.
- Virtual hardware files for GPIO and RGB LED access under `/dev/gpio` and `/dev/led`.
- WAMR payload execution through `iwasm exec`, including argument forwarding.
- Shared, binary-safe payload ABI in `sampleapps/include/webos.h`.
- One-command sample build, upload, and execution through `webdb app run`.
- Working `hello`, `blink`, `native_blink`, and `led_colors` WASM samples.
- Firmware OTA through MCUboot, delayed image confirmation, component health reporting, logs, and remote shell access.
- Automatic serial-port selection when one `/dev/tty.usbserial-*` device is connected.

The device validation flow currently completes with all startup components reporting `0`:

```text
Startup: filesystem=0 devfs=0 gpio=0 led=0 iwasm=0 wifi=0 http=0
```

## Direction

WebOS is intended to become a small WebAssembly host for ESP devices:

- Zephyr RTOS as the base firmware
- ESP32-S3 with PSRAM as the first target
- WAMR `iwasm` as the WASM runtime
- host-side AOT compilation for payloads
- FatFS for payload storage
- a small first ABI for GPIO, sleep, and logging

See `docs/idea.md` for the architecture notes and MVP constraints.

## Repository Layout

This repository is a Zephyr workspace application. In the local west workspace, it lives at:

```text
/Users/phuc/Work/webos/webos
```

Important paths:

```text
app/              Zephyr application entry point
app/src/main.c    firmware entry point and service initialization
docs/idea.md      product and architecture direction
west.yml          west manifest for Zephyr and required modules
boards/           out-of-tree board support, if needed
drivers/          out-of-tree drivers
lib/              out-of-tree libraries
tests/            Twister tests
```

The west workspace root is one level above this repository:

```text
/Users/phuc/Work/webos
```

Generated build output should live at:

```text
/Users/phuc/Work/webos/build
```

## Prerequisites

Use a Zephyr development environment with:

- `west`
- CMake and Ninja
- Zephyr SDK with ESP32-S3 Xtensa toolchain support
- Python virtual environment with Zephyr dependencies
- `ccache` for faster rebuilds
- ESP flashing tools, including `esptool`

This workspace expects the helper environment at `/Users/phuc/Work/webos/.env` to activate the Python venv and load `webos/app/.env`.

WASM sample payloads use WASI SDK 34. The local default is:

```text
~/.local/share/wasi-sdk
```

Override it with `WASI_SDK=/path/to/wasi-sdk` when needed.

## Workspace Setup

From the workspace root:

```sh
cd /Users/phuc/Work/webos
west init -l webos
west update
```

Then load the workspace environment:

```sh
source .env
```

This sets the main local variables:

```text
WEBOS_APP_DIR=/Users/phuc/Work/webos/webos/app
WEBOS_BUILD_DIR=/Users/phuc/Work/webos/build
WEBOS_BOARD=webos_esp32s3/esp32s3/procpu
```

The default target is the repository board `webos_esp32s3/esp32s3/procpu`.

## Build

After sourcing the environment:

```sh
build
```

For a clean rebuild:

```sh
rebuild
```

For debug config:

```sh
build -- -DEXTRA_CONF_FILE=debug.conf
```

The build helper enables Zephyr ccache by default with `USE_CCACHE=1`. To disable it for one build:

```sh
USE_CCACHE=0 build
```

## Flash And Monitor

Flash the current build:

```sh
flash
```

Build and flash:

```sh
run
```

Open the ESP monitor:

```sh
monitor
```

The default serial settings are:

```text
WEBOS_PORT=/dev/tty.usbserial-130
WEBOS_BAUD=115200
```

Override them before sourcing `.env` or before running the helper:

```sh
WEBOS_PORT=/dev/tty.usbserial-0001 flash
```

If the configured port is absent and exactly one `/dev/tty.usbserial-*` device is connected, `flash` and `monitor` select it automatically.

## Configuration

Open Zephyr menuconfig for the current build directory:

```sh
menuconfig
```

The application config is in:

```text
app/prj.conf
app/debug.conf
```

Application features are configured in `app/prj.conf`; local Wi-Fi credentials belong in the ignored `app/wifi.conf` fragment.

## Payload Development

All payloads include the shared native ABI in `sampleapps/include/webos.h` and use `sampleapps/common.mk`. Build an individual payload with:

```sh
make -C webos/sampleapps/led_colors
```

For the normal build, upload, and execute loop, run this from the workspace root:

```sh
tools/webdb/target/debug/webdb app run webos/sampleapps/blink
tools/webdb/target/debug/webdb app run webos/sampleapps/led_colors -- 3
```

The command builds the sample, uploads it to `/STORAGE:/apps/<name>.wasm`, runs it through `iwasm exec`, and prints the payload output. Arguments after `--` are forwarded to the payload, with the payload path provided as `argv[0]`.

Useful device checks include:

```sh
tools/webdb/target/debug/webdb shell fs ls /dev
tools/webdb/target/debug/webdb rgbled red --pin 48
tools/webdb/target/debug/webdb log --follow
```

The shared filesystem ABI is binary-safe:

```c
int dev_fs_write(const char* path, const void* data, unsigned int length);
int dev_fs_read(const char* path, void* data, unsigned int capacity);
```

`dev_fs_read()` returns the number of bytes read and does not append a string terminator.

## Testing

Run the app Twister build:

```sh
twister_app
```

Run repository tests:

```sh
west twister -T webos/tests -v --inline-logs --integration
```

## Cleaning

Remove the active build directory:

```sh
clean
```

This removes `/Users/phuc/Work/webos/build` when the standard environment is loaded.

## Current Firmware Behavior

The current app entry point is `app/src/main.c`. It initializes storage, devfs devices, WAMR, Wi-Fi, and the HTTP management service, then logs one startup status line with each component result.

## MVP Constraints

Keep early implementation choices aligned with `docs/idea.md`:

- first target is ESP32-S3 with PSRAM
- prefer WAMR AOT payloads built by the host-side compiler
- do not add an OS memory ABI such as `os->mem` or `os->malloc()` for MVP
- sample payloads are installed under `/STORAGE:/apps/<name>.wasm`
- keep the ABI minimal: GPIO, sleep, logging, and explicit-length filesystem I/O
- FatFS is the planned filesystem
- USB mass-storage mode must be exclusive with payload execution and filesystem-changing HTTP operations

## Notes For Contributors

- Treat this repo as the application/module repo, not the Zephyr tree itself.
- Keep generated build output out of `webos/app`; use `/Users/phuc/Work/webos/build`.
- Add required Zephyr modules to `west.yml` before using subsystems that need external module repositories.
- Prefer small, direct changes while the MVP is still being shaped.
