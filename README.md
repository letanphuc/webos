# WebOS

WebOS is an offline-first application runtime for ESP32-S3 devices. It keeps a stable Zephyr host firmware on the device while small WebAssembly applications are built, installed, and replaced without reflashing the operating system.

The project combines Zephyr, WAMR, persistent FatFS storage, virtual hardware files, MCUboot OTA, and the `webdb` development tool into a fast embedded application workflow.

```text
edit app -> webdb app run -> upload WASM -> execute on device -> read logs
```

## What Works Today

- ESP32-S3 firmware with 8 MB PSRAM and MCUboot
- Wi-Fi station connection and local HTTP management
- Persistent flash-backed FatFS volume mounted at `/STORAGE:`
- WAMR interpreter and AOT runtime support
- WASM applications loaded from `/STORAGE:/apps/`
- GPIO exposed through `/dev/gpio/<pin>/value` and `/direction`
- RGB LED exposed through `/dev/led/<pin>/color`
- Binary-safe filesystem ABI shared by all sample applications
- Firmware OTA, buffered logs, remote shell, and component health reporting
- One-command sample build, upload, and execution with `webdb`

The current firmware and the `hello`, `blink`, and `led_colors` applications have been validated on a physical ESP32-S3 development device.

## Developer Experience

From the west workspace root, the normal application loop is:

```sh
source .env
tools/webdb/target/debug/webdb app run webos/sampleapps/blink
```

To pass arguments to an application:

```sh
tools/webdb/target/debug/webdb app run webos/sampleapps/led_colors -- 3
```

`webdb app run` performs the complete loop:

1. Builds the selected sample with WASI SDK.
2. Uploads `<name>.wasm` to `/STORAGE:/apps/<name>.wasm`.
3. Runs it with `iwasm exec`.
4. Prints the application output.

No host firmware rebuild or device reflash is required.

## Architecture

```text
Host
+---------------------------+
| webdb                     |
| build / push / run / log  |
+-------------+-------------+
              | local HTTP
Device        v
+---------------------------+
| Zephyr host firmware      |
|                           |
|  HTTP / OTA / shell       |
|  FatFS       /STORAGE:    |
|  devfs       /dev         |
|  WAMR        iwasm        |
|  MCUboot     firmware OTA |
+-------------+-------------+
              |
              v
+---------------------------+
| WASM application          |
| webos.h native ABI        |
+---------------------------+
```

The host firmware owns networking, storage, hardware drivers, recovery, and application execution. WASM payloads remain small and disposable.

## Repository Layout

This repository is the application/module repository inside a west workspace:

```text
webos/
├── app/                    Zephyr firmware application
│   └── src/
│       ├── hal/            Hardware abstraction
│       ├── services/       Filesystem, HTTP, OTA, WAMR, logs
│       └── utils/          Shared utilities
├── boards/                 Out-of-tree ESP32-S3 board
├── drivers/                GPIO and RGB LED devfs wrappers
├── dts/                    Devicetree bindings
├── lib/devfs/              Virtual `/dev` filesystem
├── sampleapps/             WASM application examples and SDK
├── doc/                    Project documentation and product plan
├── docs/                   Architecture and implementation notes
└── tests/                  Zephyr Twister tests
```

The workspace also contains Zephyr, MCUboot, required modules, build output, and `tools/webdb`.

## Prerequisites

Install:

- west
- CMake and Ninja
- a Zephyr SDK with the ESP32-S3 Xtensa toolchain
- Python with the Zephyr dependencies
- ccache
- esptool
- Rust and Cargo for `webdb`
- WASI SDK 34 for sample applications

Sample Makefiles use this WASI SDK location by default:

```text
~/.local/share/wasi-sdk
```

Override it when necessary:

```sh
WASI_SDK=/path/to/wasi-sdk make -C webos/sampleapps/blink
```

## Workspace Setup

Create or update the west workspace from its root:

```sh
west init -l webos
west update
source .env
```

The environment configures:

```text
WEBOS_BOARD=webos_esp32s3/esp32s3/procpu
WEBOS_APP_DIR=<workspace>/webos/app
WEBOS_BUILD_DIR=<workspace>/build
```

Put local Wi-Fi credentials in the ignored `webos/app/wifi.conf` configuration fragment.

Build the device bridge once from the workspace root:

```sh
cargo build --manifest-path tools/webdb/Cargo.toml
```

## Build And Flash

After loading the workspace environment:

```sh
build       # incremental firmware build
rebuild     # pristine firmware build
flash       # flash the existing build
run         # build and flash
monitor     # open the serial monitor
menuconfig  # edit Zephyr configuration
```

Set a serial port explicitly when needed:

```sh
WEBOS_PORT=/dev/tty.usbserial-0001 flash
```

If the configured port is missing and exactly one `/dev/tty.usbserial-*` device exists, `flash` and `monitor` select it automatically.

## Build A WASM Application

All sample applications use the shared ABI header at `sampleapps/include/webos.h` and build rules from `sampleapps/common.mk`.

Build one directly:

```sh
make -C webos/sampleapps/blink
```

Available examples:

| Application | Purpose |
| --- | --- |
| `hello` | Logs a Fibonacci sequence |
| `blink` | Blinks a GPIO through devfs |
| `native_blink` | Alternative GPIO/devfs example |
| `led_colors` | Cycles the RGB LED through color combinations |

## Native Application ABI

The current SDK exports GPIO, timing, logging, and explicit-length filesystem operations:

```c
int gpio_set(unsigned int pin, unsigned int value);
int gpio_get(unsigned int pin);
void sleep_ms(unsigned int ms);
void log_print(const char* message);
int dev_fs_write(const char* path, const void* data, unsigned int length);
int dev_fs_read(const char* path, void* data, unsigned int capacity);
```

`dev_fs_read()` returns the number of bytes read and does not append a string terminator. Applications should reserve and append their own terminator when treating the result as text.

## Device Commands

Useful `webdb` commands from the workspace root:

```sh
# Inspect virtual hardware
tools/webdb/target/debug/webdb shell fs ls /dev

# Run a shell command
tools/webdb/target/debug/webdb shell kernel uptime

# Control the RGB LED
tools/webdb/target/debug/webdb rgbled red --pin 48
tools/webdb/target/debug/webdb rgbled off --pin 48

# Read or follow buffered logs
tools/webdb/target/debug/webdb log
tools/webdb/target/debug/webdb log --follow

# Upload a firmware update
tools/webdb/target/debug/webdb ota build/app/zephyr/zephyr.signed.bin
```

## Filesystem And Hardware Model

WebOS uses two primary filesystem namespaces:

```text
/STORAGE:/apps/       Installed WASM payloads
/STORAGE:/config/     Device and application configuration
/STORAGE:/logs/       Persistent log data
/STORAGE:/ota/        Update-related files
/STORAGE:/www/        Web assets
/dev/gpio/            GPIO devices
/dev/led/             LED devices
```

Examples:

```text
/dev/gpio/2/value
/dev/gpio/2/direction
/dev/led/48/color
```

The same device paths are available to WASM applications, the Zephyr shell, HTTP file operations, and `webdb`.

## Health And Recovery

At startup, WebOS reports the result of every required component:

```text
Startup: filesystem=0 devfs=0 gpio=0 led=0 iwasm=0 wifi=0 http=0
```

The `/health` response includes the same component status. MCUboot test images are confirmed only after the required local services initialize successfully, preserving rollback when a new firmware image cannot start correctly.

## Testing

Run the application integration build:

```sh
twister_app
```

Run repository tests:

```sh
west twister -T webos/tests -v --inline-logs --integration
```

Format C and C++ sources before committing:

```sh
formatcode
```

## Project Direction

The current implementation proves the deploy-and-run vertical slice. The next major milestone is a managed application lifecycle with:

- persistent application records
- start, stop, status, and autostart operations
- a dedicated supervisor thread
- crash handling and bounded restart
- staged package installation
- version activation and rollback
- logical hardware capabilities instead of physical pin assumptions

See `doc/plan.md` for the product plan, `docs/idea.md` for the original architecture direction, and `docs/ram-load-ota.md` for OTA experiments and findings.

## License

Apache-2.0. See `LICENSE`.
