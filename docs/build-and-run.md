# Build And Run WebOS

This guide covers workspace setup, firmware builds, flashing, and the
WebAssembly application loop. Commands run from the **west workspace root**
unless noted otherwise. In a normal checkout, that root contains `webos/`,
`zephyr/`, `modules/`, and `tools/wdb/`.

## Requirements

Install the host tools required by Zephyr and this project:

- Git, CMake, Ninja, and `west`
- a Zephyr SDK with the ESP32-S3 Xtensa toolchain
- Python and the Zephyr Python dependencies
- `ccache` and `esptool`
- Rust and Cargo (for WDB and Rust applications)
- WASI SDK 34 (for C applications)
- Android SDK Platform Tools only if using the optional ADB transport

C sample Makefiles look for WASI SDK at `~/.local/share/wasi-sdk` by default.
Set `WASI_SDK` to use another location:

```sh
export WASI_SDK=/path/to/wasi-sdk
```

Rust samples may require the WebAssembly target:

```sh
rustup target add wasm32-unknown-unknown
```

## Create The Workspace

Clone WebOS as the manifest repository, then let west fetch Zephyr, WAMR,
MCUboot, WDB, and the required modules:

```sh
mkdir webos-workspace
cd webos-workspace
git clone https://github.com/webos-esp/webos.git webos
west init -l webos
west update
west zephyr-export
```

Install Zephyr's Python dependencies using the environment and process
recommended by the Zephyr getting-started guide. Activate that environment
before using `west`, WDB firmware commands, or the helper functions described
in this document.

## Configure The Environment

The local development workspace may provide a root `.env` file. If it does,
load it from the workspace root:

```sh
source .env
```

Otherwise, load the tracked application environment from the directory it was
designed for, then return to the workspace root:

```sh
cd webos/app
source .env
cd ../..
```

This defines the board, build directory, serial settings, and helper commands.
The default target is:

```text
webos_esp32s3/esp32s3/procpu
```

Create `webos/app/wifi.conf` with local credentials. The file is ignored by Git:

```conf
CONFIG_WEBOS_WIFI_SSID="your-network"
CONFIG_WEBOS_WIFI_PSK="your-password"
```

You can also override credentials through `WEBOS_WIFI_SSID` and
`WEBOS_WIFI_PSK` in the environment.

## Install WDB

WDB is fetched into `tools/wdb` by west. Install the command-line tool with
Cargo:

```sh
cargo install --path tools/wdb --force
wdb --version
```

For WDB development, build and run the local binary without installing it:

```sh
cargo build --manifest-path tools/wdb/Cargo.toml
tools/wdb/target/debug/wdb --help
```

`wdbd` starts automatically when a command needs it. The daemon is the sole
owner of the device's serial port, which prevents monitor and flash commands
from racing each other and preserves logs through resets.

## Build And Flash Firmware

Connect the ESP32-S3 development board, then check discovery:

```sh
wdb devices
wdb status
```

Use WDB for the normal firmware workflow:

```sh
wdb build              # incremental build
wdb build --pristine   # clean build
wdb flash              # flash the existing build
wdb flash --follow     # flash and continue printing logs
wdb run                # build, flash, wait for startup, and follow logs
```

For multiple attached devices, select a serial path explicitly when starting
the daemon:

```sh
wdb kill-server
wdb -s /dev/tty.usbserial-0001 start-server
```

The environment also provides lower-level helpers:

```sh
build       # west incremental build
rebuild     # pristine west build
flash       # flash WEBOS_BUILD_DIR through WEBOS_PORT
run         # build, then flash
monitor     # direct Zephyr serial monitor
menuconfig  # open Kconfig configuration
```

Do not run `monitor`, another terminal emulator, or `esptool` while `wdbd` owns
the same serial port. Prefer `wdb logcat --follow`. If direct monitoring is
necessary, stop the daemon first with `wdb kill-server`.

Override the serial device when auto-discovery is not sufficient:

```sh
export WEBOS_PORT=/dev/tty.usbserial-0001
export WEBOS_BAUD=2000000
```

## Build And Run An Application

WebOS applications are independent WASM payloads. The fastest development loop
builds, uploads, and executes one in a single command:

```sh
wdb app run webos/sampleapps/hello
wdb app run webos/sampleapps/blink
wdb app run webos/sampleapps/led_colors -- 3
```

Everything after `--` is passed to the application. The host firmware stays in
place during this loop.

To build an application without deploying it:

```sh
make -C webos/sampleapps/hello
make -C webos/sampleapps/blink
make -C webos/sampleapps/rust_hello
```

To upload and launch a prebuilt payload manually:

```sh
wdb push webos/sampleapps/hello/hello.wasm /STORAGE:/apps/hello.wasm
wdb shell iwasm exec /STORAGE:/apps/hello.wasm
```

Available examples include:

| Application | Language | Demonstrates |
| --- | --- | --- |
| `hello` | C | Logging and the minimal application shape |
| `rust_hello` | Rust | A small Rust application |
| `blink` | C | GPIO through `/dev` |
| `native_blink` | C | Alternative GPIO/devfs access |
| `led_colors` | C | RGB LED control |
| `sudoku` | C | Outbound HTTPS |
| `rust_sudoku` | Rust | Outbound HTTPS from Rust |

The Sudoku examples need an API key supplied at run time:

```sh
export API_NINJAS_KEY=your-api-key
wdb app run webos/sampleapps/sudoku -- "$API_NINJAS_KEY"
```

## Inspect And Debug

Use WDB for logs, shell access, health, and hardware inspection:

```sh
wdb logcat
wdb logcat --follow
wdb health
wdb shell kernel uptime
wdb shell fs ls /dev
wdb shell fs ls /STORAGE:/apps
wdb rgbled red --pin 48
wdb rgbled off --pin 48
```

WDB prefers HTTP for structured runtime commands and uses the serial daemon for
flashing, recovery, and complete boot logs. Diagnose either route explicitly:

```sh
wdb shell --via http kernel uptime
wdb shell --via serial kernel uptime
```

Run `wdb <command> --help` for command-specific options.

## Optional ADB Transport

The firmware can expose a development-only ADB-compatible interface over the
ESP32-S3 native USB port. Install Android SDK Platform Tools, then use:

```sh
adb devices -l
adb shell
adb push app.wasm /STORAGE:/apps/app.wasm
adb logcat
adb reboot
```

ADB and WDB can run at the same time: ADB uses native USB while `wdbd` owns the
UART bridge. The current ADB endpoint is unauthenticated and implements only a
bounded subset of Android's protocol. Use it only on trusted development
systems. WDB remains required for firmware builds, flashing, application
builds, and automated workflows.

## OTA

Upload a signed image from a completed sysbuild:

```sh
wdb ota build/app/zephyr/zephyr.signed.bin
```

MCUboot applies the update and preserves rollback behavior if the new firmware
cannot initialize its required services.

## Validate Changes

Run focused physical-device checks through WDB:

```sh
wdb test boot
wdb test app webos/sampleapps/blink
```

Run host-side Zephyr integration builds and tests after loading the environment:

```sh
twister_app
west twister -T webos/tests -v --inline-logs --integration
```

Check generated ABI bindings and format C/C++ code before submitting changes:

```sh
python3 webos/scripts/generate_abi.py --check
formatcode
```

## Troubleshooting

### WDB Cannot Find The Device

Check serial paths, then restart the daemon with an explicit selection:

```sh
ls /dev/tty.usbserial-*
wdb kill-server
wdb -s /dev/tty.usbserial-0001 start-server
wdb devices
```

### The Serial Port Is Busy

Only one process can own the UART. Close direct monitors and terminal programs,
or stop WDB before using a direct monitor. On macOS, identify the owner with:

```sh
lsof /dev/tty.usbserial-0001 /dev/cu.usbserial-0001
```

### HTTP Commands Are Unavailable

HTTP operations become available after Wi-Fi connects and startup completes.
Follow the boot log and inspect status:

```sh
wdb logcat --follow
wdb status
```

Confirm the credentials in `webos/app/wifi.conf`, then rebuild and flash if they
changed.

### A Configuration Change Is Not Applied

Kconfig, devicetree, partition, Zephyr, and MCUboot changes can require a clean
build:

```sh
wdb build --pristine
wdb flash --follow
```
