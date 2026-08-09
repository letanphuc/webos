# WebOS

**An application platform for ESP32 devices, built on Zephyr and WebAssembly.**

WebOS separates the software that operates a device from the applications that
define its behavior. The OS image owns the hardware, networking, storage, and
recovery. Applications are small WebAssembly programs that can be deployed and
replaced without rebuilding or reflashing the firmware.

```text
edit app -> build WASM -> push -> run -> inspect logs -> repeat
```

The goal is to make embedded application development feel more like desktop
application development: the operating environment stays installed while the
application changes quickly.

> WebOS is not a browser or a general-purpose desktop OS. It is a compact,
> device-focused runtime for ESP32-S3 hardware.

## The Platform

WebOS combines three core projects:

- **Zephyr** provides the RTOS, drivers, networking, filesystems, and system
  services.
- **WAMR `iwasm`** loads and runs C or Rust applications compiled to WebAssembly.
- **WDB** provides the development bridge for building, flashing, deploying,
  running, and debugging from a host computer.

The firmware is ready with services applications commonly need:

| Service | What it provides |
| --- | --- |
| HTTP/HTTPS | Device management and bounded outbound requests from applications |
| Storage | Persistent FatFS data and application files under `/STORAGE:` |
| Hardware | File-oriented access through the virtual `/dev` filesystem |
| Runtime | Interpreted and AOT WebAssembly execution with a versioned host ABI |
| Operations | Shell, buffered logs, health reporting, firmware OTA, and MCUboot recovery |
| Connectivity | Wi-Fi plus serial and optional USB ADB development transports |

Applications depend on the stable WebOS ABI instead of linking directly to
Zephyr or board-specific drivers. The host can evolve independently, while
application payloads remain portable and disposable.

## Hardware Is A File

Inspired by Unix and Linux, WebOS represents each hardware function as a file:

```text
/dev/gpio/2/direction
/dev/gpio/2/value
/dev/led/48/color
```

An application reads and writes these paths rather than calling a board-specific
driver. Writing `out` to `direction` configures a GPIO; writing `1` to `value`
drives it high. The same interface is available to WASM applications, the
Zephyr shell, HTTP operations, WDB, and the optional ADB shell.

```text
WASM app -----+
WDB / ADB ----+--> read/write --> /dev --> Zephyr driver --> hardware
Zephyr shell -+
```

Native drivers stay in trusted firmware. Applications see stable paths and data
formats, making hardware discoverable and reducing coupling to a particular
board.

## Develop Like An App

WDB is the primary development interface. Its background daemon owns the serial
port, retains logs across resets, and coordinates device operations. A complete
application iteration is one command:

```sh
wdb app run webos/sampleapps/blink
```

WDB builds the payload, uploads it to the device, starts it with `iwasm`, and
prints its output. No firmware rebuild or serial flash is required.

ADB support is optional. It offers a familiar USB workflow for device discovery,
an interactive shell, sandboxed file upload, logs, and reboot:

```sh
adb devices -l
adb shell
adb logcat
```

The ADB implementation is intentionally a small, development-only subset; WDB
remains the complete bridge for firmware and application work.

For tool installation, firmware builds, flashing, application examples, and
troubleshooting, see **[Build and run](docs/build-and-run.md)**.

## Architecture

```text
Development host
+--------------------------------------------------+
| WDB CLI + wdbd             ADB CLI (optional)    |
+----------------------+---------------------------+
                       | serial / HTTP / USB
ESP32-S3               v
+--------------------------------------------------+
| Zephyr host firmware                              |
|                                                   |
| HTTP | Wi-Fi | shell | logs | OTA | FatFS         |
| devfs (/dev) | WAMR (iwasm) | WebOS ABI           |
+----------------------+---------------------------+
                       |
                       v
+--------------------------------------------------+
| C / Rust WebAssembly application                  |
+--------------------------------------------------+
```

The current target is an ESP32-S3 with 8 MB PSRAM. PSRAM holds the WAMR heap and
application memory, preserving internal SRAM for Zephyr, networking, USB, and
hardware-sensitive work.

## What Works Today

- ESP32-S3 host firmware with Wi-Fi, PSRAM, FatFS, and MCUboot
- WAMR interpreter and AOT runtime support
- C and Rust applications using WebOS ABI 1.0
- GPIO and RGB LED access through `/dev`
- Application-scoped filesystem and network policy contexts
- HTTP/HTTPS requests from WASM applications
- Remote shell, retained logs, health checks, file upload, and firmware OTA
- WDB-based build, flash, deploy, run, log, and physical-device test workflows
- Optional ADB-compatible access over native USB

The project currently assumes trusted developer payloads. Authentication,
package permissions, and stronger lifecycle isolation are future work.

## Repository

```text
app/              Zephyr host application
boards/           Out-of-tree ESP32-S3 board definitions
drivers/          Hardware wrappers exposed through devfs
lib/              WebOS services, devfs, and ADB support
abi/              Canonical WebOS ABI definitions
sdk/              Generated C and Rust application interfaces
sampleapps/       C and Rust WebAssembly examples
tools/wdb/        WDB checkout in the west workspace
doc/              API and design documentation
docs/             Guides and architecture notes
tests/            Host and physical-device tests
```

This repository is a Zephyr module and the manifest repository inside a west
workspace. Zephyr, WAMR, MCUboot, their required modules, and WDB are fetched by
`west update`.

## Documentation

- [Build and run](docs/build-and-run.md)
- [WebOS ABI 1.0](doc/abi-v1.md)
- [WDB design](doc/wdb.md)
- [Product plan](doc/plan.md)
- [Architecture direction](docs/idea.md)

## License

Apache-2.0. See [LICENSE](LICENSE).
