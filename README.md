# WebOS

## Introduction

WebOS is an embedded application platform for the ESP32-S3, built with Zephyr and WebAssembly. It separates the firmware that operates the device from the applications that define its behavior, so you can deploy, run, and replace an application without modifying the base source code or reflashing the whole system.

In a conventional embedded project, application logic, hardware drivers, networking, storage, and the RTOS are compiled into one firmware image. Even a small application change requires rebuilding and flashing that image. WebOS keeps those responsibilities separate:

- **Zephyr host firmware** owns boot, Wi-Fi, storage, hardware drivers, OTA, recovery, logging, and resource control.
- **WebAssembly applications** contain product behavior and are loaded from the device filesystem at runtime by WAMR.
- **[`wdb`](https://github.com/letanphuc/webos-wdb)** builds, uploads, starts, and observes applications from the development machine.

The result is an application workflow closer to deploying software than programming firmware:

```text
edit application
      -> build WASM
      -> upload over Wi-Fi
      -> execute on the device
      -> inspect logs and iterate
```

A new application does not need its own Zephyr tree, board configuration, network stack, or flash layout. It targets the small WebOS ABI, compiles to a portable `.wasm` payload, and uses services already provided by the host. The base firmware can evolve independently through MCUboot OTA, while applications can be installed or replaced in seconds.

### Hardware As Files

WebOS exposes device drivers through a virtual `/dev` filesystem. Hardware resources appear as ordinary paths instead of application-specific driver calls:

```text
/dev/gpio/2/direction
/dev/gpio/2/value
/dev/led/48/color
```

Applications control hardware by reading and writing these files. For example, writing `out` to a GPIO's `direction` file configures the pin, writing `1` to its `value` file drives it high, and writing three RGB bytes to an LED's `color` file updates the LED.

This file-oriented model creates one consistent interface across the system. The same path can be accessed by a WASM application, the Zephyr shell, an HTTP operation, or `wdb`. Drivers remain in trusted native firmware, while applications depend on stable names and data formats rather than board-specific Zephyr APIs.

```text
WASM application ─┐
Zephyr shell ──────┼── read/write ──> /dev ──> native Zephyr driver ──> hardware
wdb / HTTP ──────┘
```

The filesystem tree is dynamic: drivers register files at runtime, parent directories are created automatically, and empty directories are removed when their last device disappears. This makes device discovery straightforward and gives future hot-plug or optional hardware the same interface as built-in peripherals.

### Why WebAssembly

WebAssembly provides a compact, portable application format with a clear boundary between application code and the host firmware. WebOS uses WAMR to load applications from `/STORAGE:/apps/`, connect their imported functions to the WebOS ABI, and execute them without linking them into the Zephyr image.

For developers, that means:

- application iterations do not require a firmware rebuild or serial flash;
- multiple applications can share networking, storage, and hardware services;
- host and application releases can be versioned independently;
- applications can be written in languages that compile to WebAssembly;
- failures can be contained and managed by a future application supervisor.

WebOS is not a browser or a desktop operating system. It is a small device runtime: Zephyr provides the reliable embedded foundation, WebAssembly provides deployable applications, devfs provides a uniform hardware model, and `wdb` ties the development loop together.

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
- One-command sample build, upload, and execution with `wdb`

The current firmware and the `hello`, `blink`, and `led_colors` applications have been validated on a physical ESP32-S3 development device.

## Developer Experience

From the west workspace root, the normal application loop is:

```sh
source .env
wdb app run webos/sampleapps/blink
```

To pass arguments to an application:

```sh
wdb app run webos/sampleapps/led_colors -- 3
```

`wdb app run` performs the complete loop:

1. Builds the selected sample with WASI SDK.
2. Uploads `<name>.wasm` to `/STORAGE:/apps/<name>.wasm`.
3. Runs it with `iwasm exec`.
4. Prints the application output.

No host firmware rebuild or device reflash is required.

## Architecture

```text
Host
+---------------------------+
| wdb                     |
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

The workspace also contains Zephyr, MCUboot, required modules, build output, and [`tools/wdb`](https://github.com/letanphuc/webos-wdb). The companion `wdb` repository is fetched by west from the project entry in `west.yml`.

## Prerequisites

Install:

- west
- CMake and Ninja
- a Zephyr SDK with the ESP32-S3 Xtensa toolchain
- Python with the Zephyr dependencies
- ccache
- esptool
- Rust and Cargo for `wdb`
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
cargo build --manifest-path tools/wdb/Cargo.toml
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
| `hello` | Logs `hello world` |
| `blink` | Blinks a GPIO through devfs |
| `native_blink` | Alternative GPIO/devfs example |
| `led_colors` | Cycles the RGB LED through color combinations |

## WASM Host ABI

The current SDK exposes generic runtime helpers and explicit-length filesystem operations:

```c
void sleep_ms(unsigned int ms);
void log_print(const char* message);
int dev_fs_write(const char* path, const void* data, unsigned int length);
int dev_fs_read(const char* path, void* data, unsigned int capacity);
```

Hardware-specific native calls are intentionally not exported. WASM applications access GPIO, LEDs, and future devices only through `dev_fs_read()` and `dev_fs_write()` using paths under `/dev`. This keeps applications independent of Zephyr driver APIs and ensures that the shell, HTTP interface, `wdb`, and WASM all use the same device contract.

`dev_fs_read()` returns the number of bytes read and does not append a string terminator. Applications should reserve and append their own terminator when treating the result as text.

## Device Commands

Useful `wdb` commands from the workspace root:

```sh
# Inspect virtual hardware
wdb shell fs ls /dev

# Run a shell command
wdb shell kernel uptime

# Control the RGB LED
wdb rgbled red --pin 48
wdb rgbled off --pin 48

# Read or follow buffered logs
wdb log
wdb log --follow

# Upload a firmware update
wdb ota build/app/zephyr/zephyr.signed.bin
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

The same device paths are available to WASM applications, the Zephyr shell, HTTP file operations, and `wdb`.

## Health And Recovery

At startup, WebOS reports the result of every required component:

```text
Startup: OK
```

If a component fails, the startup record reports `FAILED` and includes each component's return code for diagnosis. The `/health` response exposes the component return codes, where zero means success. MCUboot test images are confirmed only after the required local services initialize successfully, preserving rollback when a new firmware image cannot start correctly.

## Testing

Run the complete integration test on the physical development device:

```sh
source .env
west webos test
```

This builds and flashes the firmware, waits for the device to connect, verifies the devfs tree, exercises GPIO and RGB LED file I/O, and runs the `hello` and `led_colors` WASM applications. The command leaves GPIO low and the RGB LED off.

For host-side Zephyr coverage, run the application integration build and repository test suites:

```sh
twister_app
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
