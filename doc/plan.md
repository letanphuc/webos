# WebOS Differentiation Plan

## Product Position

WebOS should not try to differentiate through Wi-Fi, OTA, a web dashboard, GPIO control, or WebAssembly alone. Those capabilities are necessary, but existing embedded platforms already provide them.

The proposed position is:

> WebOS is an offline-first application runtime for microcontrollers where the host firmware stays stable and hardware applications can be installed, stopped, upgraded, and rolled back like software packages.

The core promise is that the OS stays installed while small, disposable applications can change without rebuilding or reflashing the full firmware.

## Current Implementation Status

The first deploy-and-run vertical slice is working on the ESP32-S3 development device.

### Done

- [x] Stable Zephyr host firmware boots through MCUboot with external PSRAM.
- [x] Persistent flash-backed FatFS volume mounts at `/STORAGE:` and creates the application directory layout.
- [x] WASM payloads can be uploaded without rebuilding or reflashing the host firmware.
- [x] `webdb app run <sample-dir> -- <args...>` builds, uploads, executes, and displays payload output in one command.
- [x] WAMR loads, instantiates, and executes payloads from `/STORAGE:/apps/<name>.wasm`.
- [x] Payload arguments are forwarded correctly, including configurable sample arguments.
- [x] A shared payload SDK header and common build configuration define one native ABI for all samples.
- [x] Explicit-length `dev_fs_read` and `dev_fs_write` calls support binary data and validate WASM memory ranges.
- [x] File replacement truncates old storage content so shorter payload updates do not retain stale bytes.
- [x] Generic devfs is mounted at `/dev` and exposes GPIO through `/dev/gpio/<pin>/{value,direction}`.
- [x] RGB LED control is exposed through `/dev/led/48/color` and supports text and binary RGB writes.
- [x] The same devfs paths work from WASM payloads, the Zephyr shell, and `webdb`.
- [x] `hello`, `blink`, `native_blink`, and `led_colors` samples build with WASI SDK 34.
- [x] `hello`, `blink`, and `led_colors` have been run successfully on the physical development device.
- [x] Startup reports filesystem, devfs, GPIO, LED, WAMR, Wi-Fi, and HTTP component health.
- [x] MCUboot test-image confirmation is deferred until required local services initialize successfully.
- [x] Firmware build, flash, serial monitoring, HTTP shell, logs, binary upload, RGB LED control, and OTA tooling are available.

### Partially Done

- Hot deployment works for raw `.wasm` files, but install, stop, autostart, versioning, and rollback are not managed yet.
- Hardware is available through `/dev`, but paths still use physical pin numbers rather than logical capabilities.
- Upload replacement is reliable, but installation is not yet staged, packaged, signed, or atomic.
- WAMR execution is serialized, but it still runs synchronously from the shell rather than under a supervisor thread.
- Health and logs are available, but there is no self-describing hardware inventory or application-specific status model.

### Next Product Slice

The next implementation target is Phase 1: an application manager with persistent application records, start/stop/status commands, autostart, a dedicated execution thread, crash reporting, and bounded recovery. Package manifests and transactional installation should follow after lifecycle ownership is established.

## Differentiating Features

### 1. Hot-Deployed Hardware Applications

WebOS applications should be installed as WASM/AOT payloads without rebuilding or reflashing WebOS.

Required lifecycle operations:

- install and verify an application;
- start and stop it independently of the host OS;
- replace it with a newer version;
- automatically start the selected application after boot;
- roll back to the previous version;
- keep networking and management available if the application crashes.

This workflow should be substantially faster and safer than conventional monolithic embedded firmware development.

### 2. Hardware as a Virtual Filesystem

Expand the existing `/dev` model into the standard WebOS hardware interface. Example paths include:

```text
/dev/gpio/2/value
/dev/led/48/color
/dev/i2c/0/devices
/dev/sensors/temperature/value
/dev/relay/0/state
```

The same interface should be usable by WASM applications, the Zephyr shell, `webdb`, and the device web UI. Applications should not need direct knowledge of individual Zephyr driver APIs.

### 3. Portable Capability-Based Applications

Application packages should request logical capabilities instead of assuming physical pins and board-specific devices.

Example manifest:

```json
{
  "id": "room-light",
  "version": "1.2.0",
  "abi": "1.0",
  "capabilities": {
    "led": ["status"],
    "gpio": ["button"],
    "storage": ["private"],
    "timers": 2
  }
}
```

Device configuration can map `status` to LED 48 and `button` to GPIO 2. WebOS must enforce the declared capabilities so an application cannot access undeclared pins, devices, storage, or services.

The long-term goal is for the same application package to run on multiple WebOS boards without recompilation when the required logical capabilities are available.

### 4. Transactional Application Installation

Application installation must not damage the currently working application.

Installation flow:

1. Upload the package into a staging area.
2. Validate package format, architecture, ABI version, size, hash, signature, and requested capabilities.
3. Close and synchronize all staged files.
4. Atomically move the verified version into its final location.
5. Preserve the current version as the rollback candidate.
6. Activate the new version only after installation completes.

A reset or power failure during installation must leave either the old valid application or the new valid application, never a partially installed active application.

### 5. Application Supervisor

Run user applications under a dedicated supervisor instead of executing them synchronously from the shell.

The supervisor should:

- maintain `stopped`, `starting`, `running`, `stopping`, `failed`, and `backoff` states;
- execute WAMR in a dedicated Zephyr thread;
- enforce memory, stack, and execution limits;
- interrupt infinite loops and support bounded stop operations;
- detect traps and abnormal exits;
- restart with bounded exponential backoff;
- disable or roll back an application after repeated failures;
- keep WebOS networking, OTA, logs, and management responsive.

This failure isolation is central to treating WebOS as an operating system rather than a payload launcher.

### 6. Drop-to-Deploy USB Mode

Provide an optional deployment path that requires no IDE, CLI, account, or network connection.

Proposed experience:

1. Connect the device over USB.
2. The device appears as a removable WebOS drive.
3. Copy a `.webpkg` file into `INSTALL/`.
4. Safely eject the volume.
5. WebOS validates and installs the package.
6. Installation results are written to `STATUS/install.log`.

USB storage access and WebOS filesystem access must be mutually exclusive. Running applications and filesystem-changing network operations must stop before the volume is exposed to the host.

### 7. Self-Describing Hardware

WebOS should expose a machine-readable inventory of its hardware and runtime limits.

Example:

```json
{
  "board": "webos-esp32s3",
  "abi": "1.0",
  "memory": {
    "wasm_available": 1048576
  },
  "devices": [
    {
      "type": "rgbled",
      "name": "status",
      "path": "/dev/led/48/color"
    },
    {
      "type": "gpio",
      "name": "button",
      "path": "/dev/gpio/2/value"
    }
  ]
}
```

`webdb` should use this description for discovery, compatibility validation, configuration, diagnostics, and generated controls. Package requirements should be checked before installation begins.

### 8. Private Per-Application Storage

Each application should receive a private persistent directory such as:

```text
/apps-data/<app-id>/
```

Policy requirements:

- applications cannot read or modify another application's data;
- OS configuration remains inaccessible to applications;
- private data survives ordinary application upgrades;
- uninstall can either preserve or explicitly delete application data;
- shared storage and `/dev` access require declared capabilities.

### 9. Device-Hosted Application UI

A package may optionally include compressed HTML, CSS, and JavaScript assets. WebOS can serve the active application's UI locally.

This allows application logic and its control interface to be distributed as one package. A user should be able to control the device from a browser without installing a mobile application or depending on an external cloud service.

### 10. Offline-First Ownership

Provisioning, application deployment, operation, updates, logs, and recovery should work entirely on the local network.

The intended ownership model includes:

- no mandatory vendor account or subscription;
- no required Internet or cloud connection;
- per-device identity and owner authentication;
- local HTTPS with certificate pinning during device claiming;
- mDNS or another local discovery mechanism;
- owner-controlled application signing keys;
- a physical-presence path for recovery and factory reset.

## Signature Developer Workflow

The preferred network workflow should become:

```text
webdb discover
webdb claim <device>
webdb app build ./room-light
webdb app install room-light.webpkg
webdb app logs room-light --follow
webdb app rollback room-light
```

The equivalent offline deployment should be possible by copying `room-light.webpkg` into the USB `INSTALL/` directory and safely ejecting the volume.

Neither workflow should rebuild or reflash the WebOS host firmware.

## Initial Product Focus

The first differentiated release should combine three features:

1. **Managed application lifecycle**: install, start, stop, autostart, status, crash recovery, and rollback.
2. **Capability-based `/dev` model**: logical hardware names, permissions, and portable applications.
3. **Signed transactional `.webpkg` packages**: atomic installation of code, manifest, configuration, and optional web assets.

These features depend on one another and should be designed as a single vertical product slice rather than as unrelated additions.

## Suggested Implementation Order

### Phase 1: Application Manager

- Define the application state machine and persistent active/previous records.
- Move WAMR execution into a supervised Zephyr thread.
- Add start, stop, status, autostart, and crash reporting.
- Extend `webdb` with explicit application lifecycle commands.

### Phase 2: Package Format

- Define the `.webpkg` format and manifest schema.
- Add hash, signature, ABI, architecture, and resource validation.
- Implement staged and atomic installation.
- Add version activation and rollback.

### Phase 3: Capabilities and Portability

- Define logical device names and capability declarations.
- Enforce native ABI and `/dev` access per application.
- Add private per-application storage.
- Expose the self-describing hardware inventory.

### Phase 4: Owner Experience

- Add secure device claiming, local discovery, and authenticated management.
- Add device-hosted application UI assets.
- Implement exclusive and power-safe USB drop-to-deploy mode.

## Necessary but Non-Differentiating Infrastructure

The following remain required for a reliable product, but they should not be the primary product message:

- Wi-Fi connectivity and reconnection;
- HTTP/HTTPS APIs;
- firmware OTA and MCUboot rollback;
- GPIO and peripheral drivers;
- logging and health reporting;
- a web dashboard;
- ESP32-S3 and Zephyr support;
- the `webdb` command-line tool;
- WebAssembly runtime integration by itself.

The differentiation comes from combining these foundations with portable hardware capabilities, a managed application lifecycle, transactional rollback, and offline ownership.

## Product Message

> Build hardware applications once, install them without reflashing, and keep the device recoverable even when application code fails.
