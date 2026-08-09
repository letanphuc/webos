# Zephyr-Native ADB Device Plan

## Purpose

Add an ADB-compatible USB management transport to WebOS so a development host can discover and manage an ESP32-S3 board with the standard `adb` command-line tool.

Initial target commands are:

```sh
adb devices
adb shell "kernel uptime"
adb shell "fs ls /STORAGE:/apps"
adb push blink.wasm /STORAGE:/apps/blink.wasm
adb pull /STORAGE:/logs/device.log
adb reboot
```

This is an ADB protocol compatibility project, not an Android compatibility layer. WebOS will implement only services that have a safe and useful Zephyr equivalent.

## Feasibility Summary

The design is feasible on the current ESP32-S3 board:

- The board enables the native USB OTG controller as `zephyr_udc0`.
- Zephyr's next-generation USB device stack and ESP32-S3 DWC2 UDC driver build successfully for `webos_esp32s3/esp32s3/procpu`.
- The controller can expose the required vendor interface with one bulk OUT endpoint and one bulk IN endpoint.
- WebOS already has shell, FatFS, reboot, hardware identity, entropy, and cryptography facilities that can back a constrained ADB implementation.

The native USB connector wired to the ESP32-S3 USB D+/D- pins must be used. A connector that reaches the ESP32-S3 only through a USB-to-UART bridge cannot expose an ADB interface.

## Integration Constraint

The implementation must not modify the base Zephyr source tree or vendor HAL modules. In workspace terms, no ADB changes may be made under `zephyr/` or `modules/`. WebOS will consume only Zephyr's existing public USB device, kernel, filesystem, shell, hardware-information, entropy, and cryptography APIs.

All ADB source, Kconfig, CMake integration, public headers, application adapters, and tests must remain under `webos/`. If an upstream Zephyr limitation is discovered, document it and work around it in the WebOS module where possible; keep any unavoidable upstream proposal as a separate patch that is not required by the WebOS ADB implementation.

## Reference Implementation Assessment

The reference checkout is `../adbd-linux` at commit `7c56fbf46978ec889b6eee4124457dccdeb2b651` from <https://github.com/tonyho/adbd-linux.git>.

It is an old Linux port of Android's `system/core`, not a small portable adbd library. Its standalone build uses approximately 6,700 lines of ADB source plus Android support libraries and depends on:

- Linux ConfigFS and FunctionFS;
- device files and `ioctl` calls;
- file descriptors, `poll`, socketpairs, and pthreads;
- `fork`, `exec`, pipes, and PTYs for shell sessions;
- POSIX ownership, permissions, symlinks, xattrs, and timestamps;
- OpenSSL 1.0 RSA APIs;
- Android properties and optional Android framework services.

These dependencies make a direct source transplant inappropriate for Zephyr. The implementation remains useful as a protocol and behavior reference.

### Reuse Decision

| Area | Reference files | Decision |
| --- | --- | --- |
| Packet constants and 24-byte header | `adb/adb.h`, `adb/protocol.txt` | Reuse definitions and semantics |
| Connection and stream state machine | `adb/adb.cpp`, `adb/sockets.cpp` | Reimplement with bounded Zephyr state |
| Header, magic, length, and checksum validation | `adb/transport_usb.cpp`, `adb/transport.cpp` | Reimplement and add malformed-input tests |
| USB descriptors | `adb/usb_linux_client.cpp` | Reuse ADB interface identity only |
| Linux FunctionFS transport | `adb/usb_linux_client.cpp` | Replace with Zephyr USBD class |
| FD event loop and socketpairs | `adb/fdevent.cpp`, `adb/transport.cpp` | Replace with queues, mutexes, and USB callbacks |
| Service routing | `adb/services.cpp` | Reimplement as a small allowlisted table |
| Linux shell/PTY | `adb/shell_service.cpp` | Replace with Zephyr shell execution |
| Legacy sync wire protocol | `adb/SYNC.TXT`, `adb/file_sync_service.cpp` | Reuse protocol; rewrite filesystem handlers |
| RSA authentication | `adb/adb_auth_client.cpp`, `libcrypto_utils/` | Reimplement with Zephyr entropy and mbedTLS/PSA |
| Android-only services | remount, root, verity, JDWP, backup, framebuffer | Do not implement |

The copied protocol definitions and any derived code must retain the applicable Apache-2.0 notices.

## Scope

### Version 1

Version 1 will provide:

- USB-only transport;
- one attached host;
- ADB `CNXN`, `AUTH`, `OPEN`, `OKAY`, `WRTE`, and `CLSE` messages;
- a 4 KiB maximum ADB payload;
- a fixed number of logical streams;
- authenticated, non-interactive `shell:` commands;
- legacy `sync:` operations for `STAT`, `LIST`, `SEND`, and `RECV`;
- `reboot:`;
- a stable USB serial number derived from Zephyr hardware information;
- development diagnostics and protocol counters.

### Explicit Non-Goals

Version 1 will not provide:

- Android application installation or `adb install`;
- Android package manager, Binder, JDWP, backup, framebuffer, root, remount, or verity services;
- arbitrary device-node access;
- TCP ADB on port 5555;
- port forwarding or reverse forwarding;
- PTY-compatible interactive shell behavior;
- USB mass storage at the same time as ADB unless a later composite-device design explicitly permits it;
- unauthenticated shell or filesystem management in production builds.

## Proposed Architecture

Put the reusable ADB module in `webos/lib/adb`, inside the WebOS repository and alongside other WebOS Zephyr libraries such as `webos/lib/devfs`. Do not place it in the Zephyr source tree or in the board directory. The board describes the USB controller, while the library owns the ADB protocol and USB function.

Keep WebOS-specific policy adapters in the application services layer:

```text
webos/include/webos/adb.h           Public module API

webos/lib/adb/
|-- CMakeLists.txt                  Zephyr library registration
|-- Kconfig                         CONFIG_WEBOS_ADB and resource limits
|-- adb_internal.h                  Private types and invariants
|-- adb.c                           Lifecycle and connection state
|-- adb_protocol.c                  Framing, validation, checksum, CNXN/AUTH
|-- adb_stream.c                    Bounded stream table and flow control
|-- adb_services.c                  Service registry and OPEN routing
|-- adb_auth.c                      Authentication protocol and crypto adapter
`-- transports/
    `-- adb_usb.c                   Zephyr USBD class and endpoint queues

webos/app/src/services/adb/
|-- adb_service.h                   WebOS integration API
|-- adb_service.c                   Initialization and health/status wiring
|-- adb_shell.c                     Allowlisted Zephyr shell adapter
|-- adb_sync.c                      Sandboxed /STORAGE: sync adapter
|-- adb_reboot.c                    Allowed reboot-mode adapter
`-- adb_keys.c                      Authorized-key storage policy

webos/tests/adb/
|-- CMakeLists.txt
|-- prj.conf
|-- testcase.yaml
`-- src/                            Protocol and fake-transport ztests
```

Register the module with:

- `rsource "adb/Kconfig"` in `webos/lib/Kconfig`;
- `add_subdirectory_ifdef(CONFIG_WEBOS_ADB adb)` in `webos/lib/CMakeLists.txt`;
- `zephyr_library()` and `zephyr_library_sources(...)` in `webos/lib/adb/CMakeLists.txt`;
- the public header under `webos/include/webos/`, which the WebOS module root already exports to Zephyr builds.

This boundary keeps the ADB wire implementation reusable across boards and testable with a fake transport. It also keeps WebOS security decisions, filesystem policy, shell access, and reboot behavior out of the generic protocol library. Milestone 0 should primarily touch `webos/lib/adb`, `webos/include/webos/adb.h`, application initialization, and Kconfig; shell and sync adapters are added only in their later milestones.

### Threading Model

Use a bounded design rather than reproducing Linux adbd's thread-per-service and FD event loop:

1. The USB class keeps one bulk OUT request queued while connected.
2. Completed OUT data enters a small RX queue or ring buffer.
3. One ADB worker parses headers and payloads, then advances connection or stream state.
4. Services produce response chunks into a bounded TX queue.
5. The USB IN completion callback releases buffers and schedules the next chunk.
6. A mutex protects stream allocation and transmit ordering where callbacks and the worker overlap.

No service may perform unbounded work in a USB callback.

### Resource Limits

Initial limits should be compile-time configurable and conservative:

| Resource | Initial limit |
| --- | --- |
| Negotiated ADB payload | 4096 bytes |
| Simultaneous streams | 4 |
| USB RX buffers | 2 x 4096 bytes |
| USB TX buffers | 2 x 4096 bytes |
| Sync transfer buffer | 4096 bytes |
| Service destination length | 256 bytes |
| Sync path length | 256 bytes |
| Authentication token | 20 bytes |

Do not copy the reference implementation's 256 KiB `apacket` allocation or 64 KiB per-sync-service buffer. Large files must be streamed through fixed buffers.

## USB Device Function

Implement an ADB vendor-class function using Zephyr's next-generation USB device stack.

Required interface descriptor:

| Field | Value |
| --- | --- |
| Interface class | `0xff` |
| Interface subclass | `0x42` |
| Interface protocol | `0x01` |
| Endpoints | one bulk OUT, one bulk IN |
| Full-speed max packet size | 64 bytes |

Use the Zephyr loopback class in `zephyr/subsys/usb/device_next/class/loopback.c` as the structural example for descriptors, endpoint allocation, enable/disable callbacks, and completed-request handling.

USB identity requirements:

- use a project-owned or otherwise valid development VID/PID policy;
- expose meaningful manufacturer and product strings;
- derive a stable serial number through `CONFIG_HWINFO`;
- handle reset, disconnect, endpoint cancellation, and reconnect without rebooting;
- never retain logical ADB streams after a USB disconnect.

Host compatibility must be documented for macOS, Linux, and Windows. Linux may require a udev rule, Windows may require a WinUSB driver/INF, and some ADB distributions may require custom vendor-ID or libusb configuration.

## ADB Protocol Engine

### Packet Validation

Every incoming packet must be rejected before dispatch if:

- the command is unknown for the current state;
- `magic != command ^ 0xffffffff`;
- `data_length` exceeds the negotiated or local limit;
- the payload checksum is invalid for protocol versions that require it;
- stream IDs are zero where the message requires nonzero IDs;
- an `OKAY`, `WRTE`, or `CLSE` refers to an unknown or mismatched stream;
- the payload is not correctly terminated where a service name requires a terminator.

Disconnect or reset protocol state after repeated malformed packets. Never use an untrusted length for stack allocation or unchecked pointer arithmetic.

### Connection Handshake

On USB enable:

1. Reset all protocol and stream state.
2. Receive the host `CNXN` packet.
3. Negotiate the lower protocol version and payload size.
4. Authenticate the host when security is enabled.
5. Send a device banner such as:

```text
device::ro.product.name=webos;ro.product.model=WebOS_ESP32-S3;ro.product.device=webos_esp32s3;features=
```

Do not advertise optional ADB features until their complete semantics are implemented. In particular, omit `shell_v2`, `sendrecv_v2`, and related sync features in the first milestone so the host selects compatible legacy behavior.

### Stream Handling

Maintain a fixed stream table containing:

- local ID;
- host/remote ID;
- service type;
- service-specific context;
- whether a `WRTE` is awaiting `OKAY`;
- open, closing, or closed state;
- inactivity deadline.

Allocate monotonically increasing nonzero local IDs and safely handle wraparound. Allow only one unacknowledged `WRTE` per stream unless compatibility testing proves a larger window is necessary.

## Authentication And Security

Authentication is required before shell, sync, or reboot services become available in production.

ADB legacy authentication uses:

- a 20-byte random challenge;
- an RSA-2048 host key;
- PKCS#1 v1.5 signature verification using SHA-1 for protocol compatibility;
- Android's serialized ADB public-key format.

Implementation requirements:

- generate challenges from Zephyr's entropy/TRNG APIs;
- verify signatures with mbedTLS or PSA rather than OpenSSL;
- store pre-provisioned authorized host keys in a protected configuration location;
- support key replacement and revocation;
- use constant-time library verification primitives;
- rate-limit failed authentication attempts;
- clear challenges and temporary authentication state on disconnect;
- do not silently fall back to unauthenticated mode.

An unauthenticated prototype may exist only behind an explicit development Kconfig option. The build must warn clearly when enabled, and release configuration must prevent it.

Unknown host-key enrollment is out of scope until WebOS has a trustworthy physical confirmation or user-interface flow. The Android framework approval socket used by Linux adbd must not be imitated with automatic approval.

## Service Design

### Shell Service

Map `shell:<command>` to the existing Zephyr shell registry rather than `/bin/sh`.

Initial behavior:

- support non-interactive commands only;
- serialize access to the existing dummy shell backend;
- capture bounded output;
- send output using one or more ADB `WRTE` packets;
- close the stream after command completion;
- impose command length, output length, execution time, and inactivity limits;
- reject commands not included in an explicit management allowlist.

The shell adapter and HTTP shell handler must share one locking and execution abstraction rather than independently racing for the dummy backend.

Do not advertise `shell_v2` initially. A later implementation may add explicit stdout, stderr, and exit-status frames without attempting to emulate a Unix PTY.

### Sync Service

Implement the legacy sync subprotocol for:

- `STAT`: return conservative file metadata;
- `LIST`: enumerate a directory;
- `SEND`: stream a host file to the device;
- `RECV`: stream a device file to the host;
- `QUIT`: close the sync service.

Filesystem policy:

- canonicalize every path before access;
- permit only `/STORAGE:` and its descendants;
- reject `..`, ambiguous separators, embedded NUL bytes, and overlong paths;
- reject `/dev`, firmware partitions, and arbitrary device paths;
- reject symlink and special-file modes unsupported by FatFS;
- translate unsupported POSIX ownership and permission fields conservatively;
- enforce free-space and maximum-file-size limits;
- stream through fixed buffers;
- write uploads to a temporary file, flush it, and rename only after successful completion;
- remove incomplete temporary files after abort, disconnect, or timeout.

Coordinate writes with WebOS application execution, HTTP uploads, OTA, and any future USB mass-storage mode so multiple owners cannot mutate the same FatFS volume concurrently.

### Reboot Service

Implement only the explicitly supported reboot targets. Reject Android-specific reboot modes unless they map to a documented WebOS/MCUboot operation. Send the service response before scheduling the reboot where protocol behavior permits it.

## Configuration

Add a dedicated Kconfig menu with options similar to:

```text
CONFIG_WEBOS_ADB
CONFIG_WEBOS_ADB_USB
CONFIG_WEBOS_ADB_AUTH
CONFIG_WEBOS_ADB_ALLOW_NO_AUTH
CONFIG_WEBOS_ADB_MAX_PAYLOAD
CONFIG_WEBOS_ADB_MAX_STREAMS
CONFIG_WEBOS_ADB_SHELL
CONFIG_WEBOS_ADB_SYNC
CONFIG_WEBOS_ADB_REBOOT
```

Dependencies should select or require the Zephyr USB device stack, DWC2 UDC, hardware information, entropy, filesystem, shell, and cryptography features as appropriate. Production configuration must not select `CONFIG_WEBOS_ADB_ALLOW_NO_AUTH`.

## Delivery Milestones

### Milestone 0: First Proof With `adb devices`

The first implementation checkpoint is deliberately narrow: connect the native ESP32-S3 USB port and make the standard host tool list the MCU as an ADB device. Do not implement shell, file transfer, or other management services before this proof works reliably.

Deliverables:

- minimal ADB USB class with the `ff/42/01` interface and two bulk endpoints;
- fixed-buffer RX/TX sufficient for ADB packet headers and `CNXN` payloads;
- `CNXN` parsing, validation, payload negotiation, and device response;
- stable USB serial formatted as `webos-esp32s3-<chip-id>`;
- a device banner identifying the product, model, and board;
- USB connect, reset, disconnect, and reconnect logging;
- host setup notes for the development operating system.

First test:

```sh
adb kill-server
adb start-server
adb devices
adb devices -l
```

Expected result, with the actual chip identifier substituted:

```text
List of devices attached
webos-esp32s3-<chip-id>    device
```

The verbose command should also identify the MCU:

```text
webos-esp32s3-<chip-id>    device product:webos model:WebOS_ESP32-S3 device:webos_esp32s3
```

Acceptance criteria:

- firmware builds for `webos_esp32s3/esp32s3/procpu`;
- the host reports the vendor interface as `ff/42/01` and two bulk endpoints;
- `adb devices` lists the MCU in `device` state with the stable WebOS serial;
- `adb devices -l` reports the WebOS product, ESP32-S3 model, and board identity;
- ten cable disconnect/reconnect cycles return the device to the list without a board reset, leaked buffers, or a changed serial.

This milestone may use no-auth mode only in a clearly marked development build. It exposes no shell, sync, reboot, or other privileged service.

### Milestone 1: Diagnostic Stream

Deliverables:

- complete header, magic, length, and checksum validation;
- `OPEN`, `OKAY`, `WRTE`, and `CLSE` stream handling;
- one harmless diagnostic service, such as a fixed version response;
- stream timeout and disconnect cleanup;
- protocol unit tests.

Acceptance criteria:

- the Milestone 0 `adb devices` checks continue to pass;
- a supported diagnostic request completes repeatedly;
- unsupported services close cleanly without executing an action;
- malformed and oversized packets are rejected without crashes or memory corruption;
- reconnect restores a clean protocol and stream state.

### Milestone 2: Authentication

Deliverables:

- Android ADB public-key parser;
- TRNG-backed challenge generation;
- RSA signature verification through mbedTLS/PSA;
- authorized-key provisioning and revocation;
- failed-attempt rate limiting and tests.

Acceptance criteria:

- an authorized host connects;
- an unknown or incorrectly signed host remains unauthorized;
- disconnect invalidates pending challenges;
- production configuration cannot expose management services without authentication.

### Milestone 3: Non-Interactive Shell

Deliverables:

- shared, serialized Zephyr shell execution service;
- command allowlist and resource limits;
- output streaming and stream closure;
- shell integration tests.

Acceptance criteria:

```sh
adb shell "kernel uptime"
adb shell "fs ls /STORAGE:/apps"
```

Both commands must return bounded output, close normally, and leave HTTP and UART shell paths functional.

### Milestone 4: File Sync

Deliverables:

- legacy sync `STAT`, `LIST`, `SEND`, `RECV`, and `QUIT`;
- strict `/STORAGE:` sandbox;
- streamed reads and atomic temporary-file uploads;
- cancellation, disk-full, and power-failure handling tests.

Acceptance criteria:

```sh
adb push blink.wasm /STORAGE:/apps/blink.wasm
adb pull /STORAGE:/apps/blink.wasm pulled-blink.wasm
```

The pulled file must match the original byte-for-byte. Traversal attempts and writes outside `/STORAGE:` must fail.

### Milestone 5: Reboot And WDB Integration

Deliverables:

- safe `reboot:` service;
- ADB capability reporting in WebOS health/status;
- optional ADB transport discovery and routing in `wdb`;
- user documentation and recovery instructions.

Acceptance criteria:

- `adb reboot` performs a controlled reboot;
- serial boot logs remain observable through `wdbd` when the hardware exposes a separate UART bridge;
- existing HTTP, serial, application, OTA, and log workflows remain operational.

## Test Strategy

### Host-Side Unit Tests

Build the protocol core for a native test target where practical and cover:

- packet encoding and decoding;
- checksum and magic validation;
- payload negotiation;
- stream allocation, lookup, acknowledgement, close, timeout, and ID wraparound;
- partial USB reads containing headers or payload fragments;
- multiple packets delivered in one USB completion;
- malformed lengths and unknown commands;
- sync command framing and path validation;
- authentication success and failure vectors.

### Zephyr Tests

Add ztests for protocol and service logic with fake transport, filesystem, shell, and authentication adapters. Tests must not require physical USB for most state-machine coverage.

### Physical-Device Tests

Use a dedicated script or `wdb` test sequence to validate:

- enumeration and stable serial identity;
- repeated connect/disconnect and host ADB server restarts;
- shell commands and large output;
- empty, small, and multi-megabyte file push/pull;
- cancellation during upload;
- full filesystem behavior;
- malformed protocol input from a host-side USB test harness;
- unauthorized hosts and repeated failed authentication;
- reboot and recovery through the UART path.

Test on current macOS development hosts first, then Linux and Windows before declaring host compatibility.

## Risks And Mitigations

| Risk | Mitigation |
| --- | --- |
| Native connector is not wired to USB OTG | Confirm schematic and connector pin routing before firmware work |
| Custom VID/PID is not discovered or lacks permissions | Define host setup, udev, WinUSB, and ADB vendor configuration early |
| USB callbacks block or exhaust buffers | Fixed pools, worker-thread dispatch, explicit backpressure |
| ADB host sends newer feature requests | Advertise only implemented features and test with supported platform-tools versions |
| 256 KiB Linux packet design exhausts MCU memory | Negotiate 4 KiB and stream through fixed buffers |
| Shell grants excessive privilege | Require authentication, allowlist commands, and impose limits |
| Sync escapes the storage sandbox | Canonical path parser plus negative tests for every operation |
| Interrupted push corrupts an application | Temporary file, flush, validation, and atomic rename |
| FatFS has conflicting owners | Central storage ownership/locking across ADB, HTTP, apps, OTA, and USB MSC |
| Legacy ADB SHA-1 authentication is undesirable | Use it only for required ADB signature compatibility and rely on RSA key trust plus physical access policy |
| Old reference code differs from modern hosts | Use current platform-tools in integration tests and treat feature negotiation as a compatibility boundary |

## Definition Of Done

The ADB USB transport is complete when:

- the board enumerates reliably through the native ESP32-S3 USB connector;
- current supported `adb` platform-tools discover a stable device identity;
- authorized hosts can run the documented shell, push, pull, and reboot commands;
- unauthorized hosts cannot open management services;
- all input and memory use is bounded;
- sync cannot access paths outside `/STORAGE:`;
- disconnects, resets, malformed traffic, and interrupted transfers recover cleanly;
- existing UART, HTTP, OTA, filesystem, WAMR, and `wdb` workflows show no regression;
- protocol, security, host setup, and recovery behavior are documented.
