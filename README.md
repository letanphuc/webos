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
- Versioned, binary-safe WebOS ABI 1.0 with generated C and Rust bindings
- Application-scoped path and network policy contexts for WASM host calls
- Bounded outbound HTTP/HTTPS requests from WASM applications
- Firmware OTA, buffered logs, remote shell, and component health reporting
- One-command sample build, upload, and execution with `wdb`

The current firmware and the `hello`, `blink`, and `led_colors` applications have been validated on a physical ESP32-S3 development device.

## Developer Experience

From the west workspace root, load the Zephyr environment and check the device connection:

```sh
source .env
wdb devices
wdb status
```

WDB starts its background daemon automatically. The daemon is the sole owner of the serial port, retains boot logs across resets, and lets flashing, log streaming, and shell commands run from separate terminals.

The normal application loop is:

```sh
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

No host firmware rebuild or device reflash is required. WDB prefers HTTP for runtime commands and falls back to serial for shell access; binary uploads require HTTP.

## Architecture

```text
Host
+---------------------------+
| wdb CLI + wdbd daemon     |
| build / flash / run / log |
+-------------+-------------+
              | serial + HTTP
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
- Rust and Cargo for `wdb` and Rust sample applications
- Rust's `wasm32-unknown-unknown` target (`rustup target add wasm32-unknown-unknown`)
- WASI SDK 34 for C sample applications

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

Install the device bridge from the west-managed checkout:

```sh
cargo install --path tools/wdb --force
wdb --version
```

The Zephyr virtual environment must be active when using WDB to build or flash. For WDB development without installing it, use `cargo build --manifest-path tools/wdb/Cargo.toml` and run `tools/wdb/target/debug/wdb` directly.

## Build, Flash, And Monitor

After loading the workspace environment:

```sh
wdb build              # incremental firmware build
wdb build --pristine   # pristine firmware build
wdb flash              # flash the existing build and wait for startup
wdb flash --follow     # flash, print the boot log, and keep following logs
wdb run                # build, flash, wait for startup, and follow logs
```

Keep a log follower open while working from other terminals:

```sh
# Terminal 1
wdb logcat --follow

# Terminal 2
wdb flash
```

Select a serial port explicitly when more than one device is attached:

```sh
wdb kill-server
wdb -s /dev/tty.usbserial-0001 start-server
```

With one attached USB serial device, WDB discovers the port and starts the daemon automatically. Do not run a separate serial monitor or esptool process beside WDB; `wdbd` must remain the sole serial owner. Use `wdb server-status` and `wdb kill-server` to inspect or stop the daemon.

## Build A WASM Application

C sample applications use the shared ABI header at `sampleapps/include/webos.h` and build rules from `sampleapps/common.mk`. Rust samples declare the same host imports with an `extern "C"` block.

Build either language directly:

```sh
make -C webos/sampleapps/blink
make -C webos/sampleapps/rust_hello
make -C webos/sampleapps/rust_sudoku
```

Available examples:

| Application | Purpose |
| --- | --- |
| `hello` | Logs `hello world` from C |
| `rust_hello` | Logs `hello from rust` from a `no_std` Rust application |
| `blink` | Blinks a GPIO through devfs |
| `native_blink` | Alternative GPIO/devfs example |
| `led_colors` | Cycles the RGB LED through color combinations |
| `sudoku` | Fetches a Sudoku puzzle through the outbound HTTPS extension in C |
| `rust_sudoku` | Fetches a Sudoku puzzle through the outbound HTTPS extension in Rust |

## WASM Host ABI

WebOS ABI 1.0 defines six hardware-neutral core calls:

```c
void webos_log(unsigned int level, const void* data, unsigned int length);
void webos_sleep_ms(unsigned int milliseconds);
int webos_read(const char* path, void* data, unsigned int capacity);
int webos_write(const char* path, const void* data, unsigned int length);
void webos_ready(void);
void webos_heartbeat(void);
```

`abi/webos-v1.yaml` is the canonical definition. `scripts/generate_abi.py` produces `sdk/c/webos.h`, `sdk/rust/webos.rs`, and the binary ABI metadata consumed by C sample builds. Run `python3 scripts/generate_abi.py --check` to detect stale generated bindings. Every v1 payload carries immutable `webos.abi` custom-section metadata; firmware validates it before instantiating the module and rejects unsupported major or newer minor versions.

All buffers use explicit lengths, reads never append a terminator, host errors use stable WebOS codes rather than Zephyr errno values, and every native memory range is validated before access. Host calls receive an application context that bounds I/O, scopes private storage, filters devfs paths, gates network origins, and records readiness and heartbeat timestamps. See `doc/abi-v1.md` for ownership, path encoding, compatibility, errors, lifecycle behavior, and extension rules.

The current `iwasm exec` shell path is a transitional development launcher: it deliberately grants `/dev`, `/STORAGE:/apps`, and HTTP/HTTPS access because it does not yet install a package manifest. Production package execution must replace those broad grants with the package ID and declared capabilities. Payloads built before ABI metadata was introduced must be rebuilt; legacy import aliases remain only as source-level migration aids.

### Outbound HTTP Extension

WASM applications can make bounded HTTP and HTTPS requests with the optional `web_http_request()` extension available in the C and Rust SDKs. Callers supply explicit lengths for the URL, headers, request body, response buffer, and response structure. Firmware limits URL, header, body, response, and timeout sizes through `CONFIG_WEBOS_WASM_HTTP_*` settings. The response reports the status code, captured body length, declared content length, and truncation flags.

The C `sudoku` and Rust `rust_sudoku` samples demonstrate HTTPS without embedding credentials in source control:

```sh
export API_NINJAS_KEY="your-api-key"
wdb app run webos/sampleapps/sudoku -- "$API_NINJAS_KEY"
wdb app run webos/sampleapps/rust_sudoku -- "$API_NINJAS_KEY"
```

HTTPS currently trusts servers chaining to Amazon Root CA 1. The extension is not part of the ABI 1.0 core, and package-derived origin grants, a broader configurable trust store, and end-to-end DNS/connect/TLS deadlines remain required before untrusted payloads are supported.

## Device Commands

Useful `wdb` commands from the workspace root:

```sh
# Inspect combined serial, boot, startup, and HTTP health
wdb devices
wdb status

# Inspect virtual hardware or run another Zephyr shell command
wdb shell fs ls /dev
wdb shell kernel uptime

# Select HTTP or serial explicitly when diagnosing a transport
wdb shell --via http kernel uptime
wdb shell --via serial kernel uptime

# Control the RGB LED
wdb rgbled red --pin 48
wdb rgbled off --pin 48

# Read retained logs or follow new serial output
wdb logcat
wdb logcat --follow

# Upload a file or signed firmware update over HTTP
wdb push app.wasm /STORAGE:/apps/app.wasm
wdb ota build/app/zephyr/zephyr.signed.bin
```

Run `wdb <command> --help` for all command-specific options. See [`tools/wdb/README.md`](https://github.com/letanphuc/webos-wdb#readme) for daemon configuration, transport behavior, and troubleshooting.

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

Run focused physical-device smoke tests through WDB:

```sh
source .env
wdb test boot
wdb test app webos/sampleapps/blink
```

Run the complete repository integration test when changing firmware or device behavior:

```sh
west webos test
```

The complete test builds and flashes through WDB, waits for the device to connect, verifies the devfs tree, exercises GPIO and RGB LED file I/O, and runs the `hello` and `led_colors` WASM applications. It requires `wdb` at `~/.cargo/bin/wdb` and leaves GPIO low and the RGB LED off.

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
