# WDB Device Bridge Design

## Purpose

WDB is the unified development frontend for WebOS devices. It hides transport details behind one command-line interface and chooses the best available backend for each operation.

WDB has two backends:

1. A connection to the `wdbd` daemon, which exclusively owns serial ports.
2. A direct connection to the WebOS HTTP server.

The frontend expresses operations such as flash, shell, push, logcat, application execution, and testing. It does not require users or scripts to decide which low-level tool should perform them.

```text
                         +--------------------------+
                         | wdbd                     |
                         | serial owner             |
                    +--->| flash / reset / logcat   |---> Serial
                    |    | serial shell / discovery |
+---------------+   |    +--------------------------+
| wdb frontend  |---|
| command router|   |    +--------------------------+
+---------------+   +--->| HTTP backend             |---> WebOS HTTP server
                         | push / shell / OTA / apps |
                         +--------------------------+
```

## Goals

- Provide one frontend for firmware, serial, HTTP, application, and test workflows.
- Prevent serial-port contention by making one daemon the only serial owner.
- Allow several WDB clients to work with the same device concurrently.
- Preserve logs across CLI invocations, resets, and flashing.
- Capture logs immediately after flashing without a monitor startup gap.
- Prefer HTTP for structured operations while retaining serial recovery paths.
- Make physical-device tests deterministic and easy to automate.
- Keep the design independent of ESP-specific user commands.

## Non-Goals

- WDB is not a replacement for Zephyr, west, esptool, or WAMR internals. It orchestrates them behind stable operations.
- The first version does not need a binary IPC protocol; framed JSON over a local socket is sufficient.
- Binary file transfer over an interactive serial shell is not supported.
- Serial and HTTP logs are not merged by default because they may contain duplicates.

## Components

### `wdb`

`wdb` is the user-facing command-line client:

```sh
wdb devices
wdb status
wdb flash
wdb logcat
wdb shell
wdb push
wdb ota
wdb app run
wdb test
```

It is responsible for:

- parsing commands and selecting a device;
- converting commands into typed operations;
- selecting a backend according to capability and availability;
- acquiring operation leases when commands may conflict;
- presenting consistent text, JSON, and test output;
- returning stable process exit codes.

The client never opens a serial port directly.

### `wdbd`

`wdbd` is a long-running background daemon. It is responsible for:

- discovering connected serial devices;
- exclusively owning serial ports;
- capturing serial output continuously;
- maintaining bounded per-device log history;
- coordinating flash, reset, and serial reconnection;
- executing serialized serial-shell transactions;
- tracking device state and boot generations;
- providing locks and leases to multiple WDB clients;
- learning HTTP addresses and firmware state from boot output.

Suggested local state:

```text
~/.wdb/
├── wdbd.sock
├── wdbd.pid
├── devices.json
└── logs/
    └── <device-id>.log
```

The client should start the daemon automatically when required. Explicit lifecycle commands remain available:

```sh
wdb start-server
wdb kill-server
wdb server-status
```

### HTTP Backend

The HTTP backend communicates with the WebOS management server. It is responsible for:

- structured shell requests and return codes;
- binary file uploads;
- firmware OTA uploads;
- application upload and execution;
- health requests;
- runtime log retrieval;
- devfs reads and writes.

The client may communicate directly with HTTP after acquiring a per-device lease from `wdbd`. This keeps transfer overhead low while allowing the daemon to prevent conflicting operations.

## Device Identity And State

WDB should identify a device using a stable firmware or chip identifier, such as its MAC address, instead of treating the changing serial path as its identity.

Example device record:

```json
{
  "id": "30:ed:a0:27:da:64",
  "serial_port": "/dev/cu.usbserial-1130",
  "serial_state": "connected",
  "http_address": "192.168.50.17:8080",
  "http_state": "ready",
  "firmware": "0.1.0+9685d89",
  "boot_id": 12
}
```

Device selection follows the familiar multi-device model:

```sh
wdb devices
wdb -s 30:ed:a0:27:da:64 status
wdb -s 30:ed:a0:27:da:64 logcat
```

Selection is automatic when exactly one device is connected.

Each device has an explicit state:

```text
disconnected
connecting
idle
building
flashing
booting
online
busy
failed
```

`wdb status` combines daemon and HTTP information:

```text
Device:       30:ed:a0:27:da:64
State:        online
Serial:       /dev/cu.usbserial-1130
Firmware:     0.1.0+9685d89
Boot:         12
Address:      192.168.50.17:8080
HTTP:         ready
Filesystem:   ready
WAMR:         ready
Last flash:   18 seconds ago
```

## Backend Capabilities

Commands express intent instead of naming a transport. Each backend advertises supported capabilities.

```text
Serial daemon:
  device-discovery
  device-state
  flash
  reset
  boot-state
  log-stream
  shell

HTTP:
  health
  shell
  push
  OTA
  app-run
  runtime-log
  devfs-read
  devfs-write
```

This capability model allows future transports without redesigning every command:

```text
USB management protocol
BLE
WebSocket
native simulator
debug probe
```

## Routing Policy

Default routing is `auto`:

1. Resolve the selected device.
2. Determine available backend capabilities.
3. Prefer HTTP for structured runtime operations.
4. Prefer the daemon for serial, flashing, boot, and complete logs.
5. Fall back only when the operation supports a safe alternative.
6. Return a clear error when the required backend is unavailable.

Users can override routing for commands that support both backends:

```sh
wdb shell --via auto kernel uptime
wdb shell --via http kernel uptime
wdb shell --via serial kernel uptime

wdb logcat --via serial
wdb logcat --via http
```

### Command Routing Table

| Command | Preferred backend | Fallback | Notes |
| --- | --- | --- | --- |
| `devices` | daemon | none | Serial discovery and cached HTTP state |
| `status` | daemon + HTTP | daemon | Combines transport and WebOS health |
| `flash` | daemon | none | Daemon coordinates serial ownership |
| `reset` | daemon | none | Uses the serial reset controls |
| `logcat` | daemon | HTTP | Serial includes boot and crash logs |
| `shell` | HTTP | daemon | HTTP provides clean response framing |
| `push` | HTTP | none | Binary transfer requires HTTP |
| `pull` | HTTP | none | Requires a future download endpoint |
| `ota` | HTTP | none | Daemon observes the resulting reboot |
| `rgbled` | HTTP | serial shell | HTTP devfs write is preferred |
| `app run` | HTTP | serial execution only | Upload always requires HTTP |
| `health` | HTTP | daemon boot state | Serial state is less detailed |
| `test` | both | step-specific | Uses the best backend for each step |

## Shell Operations

### HTTP Shell

HTTP is preferred:

```sh
wdb shell fs ls /dev
```

The backend sends a structured request and receives a defined return code and output. This avoids prompt parsing and serial-log interleaving.

### Serial Shell

Serial is a fallback and recovery path:

```sh
wdb shell --via serial fs ls /dev
```

The daemon must serialize serial-shell transactions while continuing to record all bytes in log history. An initial implementation can use prompt-based framing:

1. Acquire the serial-shell lock.
2. Recover or wait for the prompt.
3. Send the command.
4. Capture output until the next prompt.
5. Release the lock.

A future framed serial management protocol can replace prompt parsing without changing the WDB command interface.

### Safe Fallback

WDB must not automatically repeat a command after an ambiguous HTTP timeout. The server may already have executed a mutating command. Automatic serial fallback is safe only when the HTTP connection failed before the request was sent or when the operation is explicitly idempotent.

## File Transfer

`push` requires the HTTP backend:

```sh
wdb push blink.wasm /STORAGE:/apps/blink.wasm
```

If HTTP is unavailable, WDB should explain the current device state:

```text
error: push requires the HTTP backend
device serial is connected, but WebOS HTTP is not ready
last known Wi-Fi state: connecting
```

WDB must not encode arbitrary binary data into interactive serial-shell commands. Serial transfer is slow, difficult to resume, and likely to corrupt data.

Application execution is therefore a mixed operation:

```text
local build                    frontend
upload WASM                    HTTP
execute `iwasm exec ...`       HTTP preferred, serial fallback
application logs               serial daemon preferred
```

## Logcat

The daemon is the preferred log source because serial captures:

- MCUboot output;
- early Zephyr initialization;
- Wi-Fi connection attempts;
- crashes and fatal exceptions;
- resets;
- logs before HTTP is available;
- logs after HTTP fails.

Examples:

```sh
wdb logcat
wdb logcat --follow
wdb logcat --clear
wdb logcat --dump
wdb logcat --since 30s
wdb logcat --since-flash
wdb logcat --since-boot
wdb logcat --level warn
wdb logcat --tag iwasm
wdb logcat --grep blink
wdb logcat --format json
```

Automatic source policy:

```text
serial connected -> serial logcat
serial absent and HTTP ready -> HTTP log
neither available -> error
```

Serial and HTTP logs should not be merged by default because the same firmware message may appear in both streams.

### Structured Log Records

The daemon should retain structured metadata in addition to raw bytes:

```json
{
  "sequence": 1842,
  "device": "30:ed:a0:27:da:64",
  "boot_id": 12,
  "source": "serial",
  "timestamp_host": "2026-08-08T05:20:11.421Z",
  "timestamp_device_ms": 19287,
  "level": "info",
  "module": "webos",
  "message": "Startup: filesystem=0 devfs=0 gpio=0 led=0 iwasm=0 wifi=0 http=0"
}
```

Structured records make filtering and device tests reliable without repeatedly parsing ANSI terminal output.

## Flash Lifecycle

The daemon owns the complete flash-to-log transition:

1. Acquire the exclusive device lock.
2. Notify log subscribers that flashing has started.
3. Pause the serial reader and close the port.
4. Enter the chip bootloader.
5. Invoke the configured flash backend.
6. Verify the written image.
7. Reopen the serial port.
8. Begin a new boot generation in the log buffer.
9. Reset the device only after log capture is ready.
10. Parse the WebOS startup status.
11. Update the cached HTTP address and readiness.
12. Release the exclusive device lock.

The reset ordering is important. Running a raw flash command and starting a monitor afterward can lose early boot output. The daemon should flash without the final automatic reset when supported, reopen serial, then perform the reset itself.

Commands:

```sh
wdb flash
wdb flash --follow
wdb flash --wait-ready
wdb run
```

Expected output:

```text
[device] 30:ed:a0:27:da:64
[flash]  935180 bytes written and verified
[boot]   filesystem=0 devfs=0 gpio=0 led=0 iwasm=0
[http]   192.168.50.17:8080 ready
[ready]  device online in 19.3s
```

`wdb run` means build, flash, capture boot, wait for readiness, and continue displaying logs.

## Concurrency And Locks

Multiple clients may connect to one daemon. Operations are serialized only when necessary.

### Always Concurrent

- multiple `logcat --follow` subscribers;
- `status` and `devices`;
- reading retained logs;
- watching state transitions.

### Per-Device Locks

```text
serial-exclusive:
  flash
  reset

serial-shell:
  serial shell command

http-mutation:
  push
  OTA
  app run
  devfs writes
```

Log subscriptions never require an exclusive lock.

The current WebOS HTTP server safely supports one management transaction at a time, so WDB should acquire an HTTP mutation lease through the daemon before issuing a mutating request. A waiting command queues by default. `--no-wait` returns a busy error immediately.

Example with three terminals:

```text
Terminal 1: wdb logcat --follow
Terminal 2: wdb flash
Terminal 3: wdb status
```

Expected behavior:

- Terminal 1 remains subscribed and sees the flash marker and new boot.
- Terminal 2 receives flash progress and readiness events.
- Terminal 3 reports `flashing`, `booting`, and then `online`.
- No client attempts to open the serial port directly.

## IPC Protocol

Use a Unix domain socket on macOS and Linux:

```text
~/.wdb/wdbd.sock
```

Framed JSON is sufficient for the initial protocol:

```json
{"id":12,"command":"logcat","device":"30:ed:a0:27:da:64","follow":true}
```

Responses support one-shot and streaming operations:

```json
{"id":12,"type":"response","ok":true}
{"id":12,"type":"log","sequence":1842,"message":"..."}
{"id":12,"type":"end"}
```

The protocol should include a version handshake so client and daemon upgrades fail clearly instead of producing undefined behavior.

## Testing Model

WDB tests combine both backends while hiding transport details from test definitions.

Example:

```sh
wdb test app sampleapps/blink
```

Internal sequence:

1. Ask the daemon to mark the current log position.
2. Build the sample locally.
3. Upload the payload through HTTP.
4. Execute it through the HTTP shell.
5. Wait for completion in daemon-captured logs.
6. Read `/dev/gpio/2/value` through HTTP.
7. Save related serial logs as an artifact.

A declarative test could look like:

```yaml
name: blink
timeout: 20s

steps:
  - app-run: sampleapps/blink
  - expect-log: "payload: blink: starting"
  - expect-log: "payload: blink: done"
  - expect-log: "Executed /STORAGE:/apps/blink.wasm successfully"
  - shell: "fs read /dev/gpio/2/value"
    expect: "0"
```

Firmware smoke testing:

```sh
wdb test boot --flash
```

Suggested sequence:

1. Build and flash firmware through the daemon.
2. Capture logs from the first reset byte.
3. Wait for a new boot ID.
4. Assert that every startup component reports `0`.
5. Verify the HTTP health response.
6. Execute `kernel uptime`.
7. Upload and execute `hello.wasm`.
8. Assert successful payload output.
9. Save logs and results in JSON or JUnit format.

This provides stable timeouts, meaningful exit codes, and useful failure artifacts without raw serial scripts.

## Proposed Command Set

```sh
# Server and devices
wdb start-server
wdb kill-server
wdb server-status
wdb devices
wdb status
wdb wait-for-device
wdb wait-for-boot

# Firmware and serial
wdb build
wdb flash
wdb run
wdb reset
wdb logcat

# WebOS operations
wdb shell
wdb push
wdb pull
wdb ota
wdb health
wdb rgbled

# Applications
wdb app build
wdb app push
wdb app run
wdb app list

# Testing
wdb test boot
wdb test app sampleapps/blink
wdb test --all
```

## Delivery Plan

### Phase 1: Serial Ownership And Logcat

- Auto-start `wdbd`.
- Discover connected serial devices.
- Add one persistent serial reader per device.
- Add bounded log buffers and multiple subscribers.
- Implement `devices`, `status`, and `logcat`.
- Reconnect automatically when serial devices disappear and return.

Success means normal development no longer opens raw serial monitors.

### Phase 2: Flash Coordination

- Move flash orchestration into the daemon.
- Add per-device exclusive operations.
- Reacquire serial before the final reset.
- Add boot-generation markers.
- Implement `flash`, `run`, and `wait-for-boot`.
- Parse the existing WebOS startup status.

Success means boot logs are never lost between flashing and monitoring.

### Phase 3: HTTP Routing

- Move the existing shell, push, OTA, RGB LED, and application commands behind typed frontend operations.
- Add automatic HTTP-preferred routing and serial shell fallback.
- Add HTTP mutation leases.
- Track HTTP readiness and the current device address.

Success means several WDB clients can safely share a device.

### Phase 4: Device Tests

- Add log assertions and boot-health checks.
- Add application test manifests.
- Add stable timeouts and process exit codes.
- Add JSON and JUnit output.
- Preserve logs as test artifacts.

Success means physical-device integration tests run without custom serial, curl, or monitor scripts.

## Recommended First Milestone

The smallest high-value milestone is:

```sh
wdb start-server
wdb devices
wdb flash --follow
wdb logcat --since-flash
wdb wait-for-boot
```

Acceptance criteria:

- Only `wdbd` opens serial ports.
- Several clients can follow logs simultaneously.
- Flashing pauses and restores log capture automatically.
- Logs are available immediately after flashing.
- Scripts can wait for a structured startup result.
- Normal development does not require raw west flash, esptool, serial monitor, or device-path commands.

## Design Boundaries

- `wdb` owns user experience, command parsing, routing, and output.
- `wdbd` owns serial ports, discovery, retained logs, flash coordination, and locks.
- The HTTP backend owns structured WebOS request and response operations.
- Firmware-specific behavior stays behind typed operations rather than leaking endpoint details into every command.
- Users and tests operate through WDB rather than raw transport tools.
