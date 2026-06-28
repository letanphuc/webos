# WebOS

WebOS is a Zephyr-based firmware project for ESP32-S3 devices. The goal is to keep a stable host OS on the device and run small, disposable application payloads through WebAssembly.

The current tree is in bring-up. The app builds as a C++ Zephyr application and currently runs a minimal heartbeat on ESP32-S3.

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
app/src/main.cpp  current C++ firmware entry point
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
WEBOS_BOARD=esp32s3_devkitm/esp32s3/procpu
```

Zephyr currently maps `esp32s3_devkitm/esp32s3/procpu` to `esp32s3_devkitc/esp32s3/procpu` and prints a deprecation warning during configure.

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
WEBOS_PORT=/dev/tty.usbserial-1130
WEBOS_BAUD=115200
```

Override them before sourcing `.env` or before running the helper:

```sh
WEBOS_PORT=/dev/tty.usbserial-0001 flash
```

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

The app currently enables C++ support with:

```text
CONFIG_CPP=y
CONFIG_REQUIRES_FULL_LIBCPP=y
```

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

The current app entry point is `app/src/main.cpp`. It prints a startup message and a one-second heartbeat:

```text
WebOS hello from Zephyr on ESP32-S3
WebOS heartbeat
```

## MVP Constraints

Keep early implementation choices aligned with `docs/idea.md`:

- first target is ESP32-S3 with PSRAM
- prefer WAMR AOT payloads built by the host-side compiler
- do not add an OS memory ABI such as `os->mem` or `os->malloc()` for MVP
- first payload path is `/apps/blink.aot`
- first ABI should stay minimal: GPIO set, sleep milliseconds, and log print
- FatFS is the planned filesystem
- USB mass-storage mode must be exclusive with payload execution and filesystem-changing HTTP operations

## Notes For Contributors

- Treat this repo as the application/module repo, not the Zephyr tree itself.
- Keep generated build output out of `webos/app`; use `/Users/phuc/Work/webos/build`.
- Add required Zephyr modules to `west.yml` before using subsystems that need external module repositories.
- Prefer small, direct changes while the MVP is still being shaped.
