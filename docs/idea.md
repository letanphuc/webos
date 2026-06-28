# WebOS for ESP Devices: Architecture Design Note

Project: Micro-PaaS / edge-native WebAssembly operating system  
Runtime and OS: `iwasm` from WAMR on Zephyr  
Target silicon: ESP32-S3 with PSRAM  
Core philosophy: "The OS stays forever; payloads are disposable."

## 1. Executive Summary

WebOS breaks away from the traditional embedded firmware model, where each application change requires rebuilding and reflashing a monolithic image. Instead, it turns an ESP32-class microcontroller into a small host server.

The system is split into two layers:

- Host OS: static firmware containing Zephyr, networking, Wi-Fi drivers, TLS, a filesystem, a web server, and the `iwasm` runtime. This layer is flashed once and remains stable.
- Application payloads: user logic compiled to `.wasm` and then ahead-of-time compiled to `.aot`, typically around 2 KB to 20 KB. Payloads can be copied, uploaded, started, stopped, and deleted without reflashing the chip.

## 2. High-Level Architecture

```text
+-------------------------------------------------------------------------------+
|                           USER APPLICATION PAYLOAD                            |
|             Written in C / Rust / Go, compiled to .wasm / .aot                |
+---------------------------------------+---------------------------------------+
                                        |  CALL *(SYS_CALL_TABLE + index)
+---------------------------------------v---------------------------------------+
|                               WebOS ABI BRIDGE                                |
|          struct os_table_t { magic, version, size, *gpio, *wifi, ... }        |
+-------------------------------------------------------------------------------+
|                         WASM RUNTIME ENGINE: iwasm                            |
|              Sandboxed Linear Memory     Instruction Yielding                 |
+-------------------------------------------------------------------------------+
|                            Zephyr RTOS + Filesystem                           |
+-------------------------------------------------------------------------------+
|                              ESP32 Silicon                                    |
+-------------------------------------------------------------------------------+
```

## 3. Key Architecture Decisions

### ADR-01: Memory Management

Decision: do not expose an OS memory module through the ABI for MVP.

Context: system programmers often want to add APIs such as `os->malloc()` or `os->mem->alloc_dma()` to the ABI bridge. That makes ownership unclear, couples application memory directly to host memory, and adds complexity before the basic runtime model is proven.

Rules:

- Normal application memory belongs to user space and is managed by the WASM libc inside the module's linear memory.
- When an application calls `malloc()`, the WASM libc allocates from its own linear memory.
- If linear memory is exhausted, libc may call `memory.grow(1)`.
- The OS may satisfy `memory.grow(1)` by assigning one additional 64 KB WASM page to the container, if policy and available memory allow it.
- The OS does not expose `os->mem`, `os->malloc()`, `os->free()`, or `os->mem->alloc_dma()` in the MVP ABI.
- Payloads should use normal libc APIs first: `malloc()`, `free()`, stack allocation, and standard buffer handling inside WASM memory.

Rationale: this keeps application memory sandboxed and makes the MVP ABI smaller. Special memory APIs, DMA buffers, and zero-copy peripheral paths can be revisited later only when a concrete payload requires them.

### ADR-02: Function Linking Through GOT-Style Indexing

Decision: payloads must call OS services through stable table indexes, not physical function addresses.

Context: if a payload is linked against raw C function addresses, every OS firmware update can move those functions and break older payloads.

Mechanism:

- The OS owns a stable syscall table in RAM, for example: `void* SYS_CALL_TABLE[] = { &gpio_set, &gpio_get, &wifi_send };`.
- Payload calls are compiled as indexed calls, for example: `CALL *(SYS_CALL_TABLE + 0)`.
- The meaning of each index is stable across ABI-compatible OS versions.

Result: if `gpio_set` moves from address `0x4000` to `0x8000` in WebOS v2.0, it can still remain at index `0`. Existing payloads continue to run.

### ADR-03: UEFI-Style OS Table Layout

Decision: define the OS ABI table as a packed, versioned, self-describing structure.

Context: C compilers, WASM toolchains, and firmware revisions can disagree on padding, alignment, and structure layout. The ABI must make those differences explicit.

Proposed layout:

```c
#define OS_ABI __attribute__((packed, aligned(4)))

typedef struct OS_ABI {
    uint32_t magic;       // Always 0x5745424F, "WEBO"
    uint16_t abi_version; // Example: 0x0100 for v1.0
    uint16_t struct_size; // sizeof(os_table_t), checked by the app
    os_gpio_t* gpio;
    os_wifi_t* wifi;
} os_table_t;
```

Rules:

- `magic` verifies that the payload received a valid WebOS table.
- `abi_version` identifies the ABI contract.
- `struct_size` allows newer OS versions to append fields while older payloads continue to validate the part they know.
- Field order is append-only within an ABI compatibility line.

### ADR-04: AOT-First Payload Execution

Decision: prefer ahead-of-time compiled payloads from the beginning.

Context: ESP32 devices do not have enough RAM or CPU headroom to compile `.wasm` into Xtensa native code on-device. Pure interpretation is simpler, but it can be too slow for edge workloads. Since the target runtime is WAMR `iwasm`, WebOS should use WAMR's AOT path as the preferred execution mode.

Flow:

1. The developer builds `app.wasm` from C, Rust, or another supported language.
2. The WAMR/iwasm host-side AOT compiler compiles `app.wasm` into `app.aot` for ESP32-S3 using default compiler flags for MVP.
3. The user uploads `app.aot` through the WebOS GUI or API.
4. Zephyr stores the payload in the device filesystem.
5. `iwasm` loads and runs the AOT payload with near-native performance.

Goal: keep the device lightweight while allowing payloads to run much faster than interpreter mode. Interpreter mode can remain useful for bring-up and debugging, but AOT is the preferred production path. Browser-side AOT compilation is not part of the MVP.

### ADR-05: Filesystem and USB Storage Mode

Decision: use FatFS as the first device filesystem and expose it to macOS through USB mass-storage mode when USB is connected.

Context: payload deployment should be simple during early development. Since macOS can mount FAT volumes without extra tools, the device can behave like a removable drive when the user wants to manage payload files directly.

Policy:

- Store payloads and basic WebOS data on FatFS.
- Automatically enter USB storage mode when USB is connected.
- Expose the FatFS volume to the host computer while USB storage mode is active.
- While USB storage mode is active, WebOS must stop normal runtime behavior and do nothing else with the mounted filesystem.
- WASM payload execution, HTTP upload, and other filesystem writes must be paused or disabled during USB storage mode.
- Normal WebOS operation resumes only after the USB volume is unmounted and unplugged.

Rationale: this avoids filesystem corruption from simultaneous access by macOS and the device firmware.

### ADR-06: Cooperative Survival Under Zephyr

Decision: WASM execution must yield to the RTOS regularly.

Context: an infinite loop such as `while (true)` inside a WASM payload can monopolize the CPU, starve networking and system work, or trigger watchdog behavior.

Policy:

- Run the WASM runtime in a dedicated Zephyr thread.
- On ESP32-S3, prefer keeping networking and system services isolated from WASM workload as much as Zephyr and the ESP32-S3 port allow.
- Enable WAMR's reduction counter or equivalent instruction-budget mechanism.
- After a configured instruction budget, for example 10,000 WASM instructions, the runtime yields with `k_yield()`, `k_sleep()`, or an equivalent scheduler-friendly pause.

Result: misbehaving payloads should not starve networking, system services, or the watchdog.

### ADR-07: Trust Model for Early Payloads

Decision: the first version assumes trusted payloads.

Context: WebOS still benefits from WASM sandboxing, but supporting hostile third-party payloads would require more work: signatures, quotas, capability restrictions, persistence isolation, upload authorization, and stronger failure containment.

Policy for MVP:

- Payloads are written and uploaded by the device owner or trusted developer.
- The ABI is designed cleanly, but security hardening is not the first milestone.
- Resource limits should still exist where cheap to implement, especially memory size and execution budget.
- Cryptographic signing and a capability-based permission model can be added after the core runtime and deployment loop are proven.

### ADR-08: Runtime Modes

Decision: WebOS has three explicit runtime modes.

Normal mode:

- Mount FatFS internally.
- Start networking and the HTTP management server.
- Allow `/ota`, `/push`, and `/shell`.
- Allow loading, starting, stopping, and deleting trusted AOT payloads.

USB storage mode:

- Enter automatically when USB is connected to a host.
- Stop any running payload before exposing FatFS to the host.
- Stop or reject HTTP operations that touch the filesystem.
- Expose FatFS as USB mass storage.
- Keep WebOS idle until the USB volume is unmounted and unplugged.
- Resume normal mode after unplug.

OTA mode:

- Receive a firmware image through `POST /ota`.
- Store the image in a staging location.
- Verify at least size and checksum before applying.
- Apply the update using Zephyr's recommended firmware update path, preferably MCUboot if available for the target board.
- Reboot into the new firmware only after the update is accepted.

Rationale: explicit modes avoid accidental simultaneous access to FatFS and make failure behavior easier to reason about.

## 4. Memory and PSRAM Strategy

### The 64 KB WASM Page Trap

WASM memory grows in 64 KB pages. ESP32 internal SRAM is limited and can become fragmented after long uptime. The system may fail to find one contiguous 64 KB block even when the total free memory appears sufficient.

Strategy:

- ESP32-S3 with PSRAM is the primary and minimum supported target.
- Place WASM linear memory in PSRAM.
- Keep internal SRAM reserved for Zephyr, networking buffers, hardware-critical paths, and latency-sensitive system services.

## 5. Filesystem Layout

Use a small, predictable FatFS layout so both WebOS and macOS users can understand the volume.

```text
/apps/
  app.aot              # Uploaded payloads
/config/
  active_app           # Optional path to the selected payload
  webos.json           # Optional runtime configuration
/logs/
  system.log           # Optional system log
  payload.log          # Optional payload log
/ota/
  staged.bin           # Temporary firmware upload before validation
/www/
  index.html.gz        # Optional compressed web UI assets
```

MVP defaults:

- LED blink payload path: `/apps/blink.aot`.
- Do not autostart payloads until manual start works reliably.
- If autostart is added later, read `/config/active_app` during normal-mode boot.

## 6. Payload Lifecycle

The first lifecycle should be deliberately small.

States:

- `stopped`: payload exists on FatFS but is not running.
- `starting`: WebOS is loading the AOT payload and creating the `iwasm` instance.
- `running`: payload is executing inside the WASM runtime thread.
- `stopping`: WebOS has requested termination and is cleaning up runtime state.
- `failed`: payload load, validation, execution, or cleanup failed.

Operations:

- Push: copy an `.aot` file to `/apps/` through `POST /push` or USB storage mode.
- Start: load a selected `.aot` file into `iwasm` and call its entry point.
- Stop: request payload termination and destroy the runtime instance.
- Delete: remove a stopped payload from FatFS.
- Replace: stop the running payload, overwrite the file, then start it again manually.

MVP rule: only one payload runs at a time.

## 7. First ABI Surface

The first ABI should support only the LED blink payload.

```c
#define WEBOS_MAGIC 0x5745424F // "WEBO"
#define WEBOS_ABI_VERSION 0x0100

typedef struct OS_ABI {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t struct_size;
    os_gpio_t* gpio;
    os_time_t* time;
    os_log_t* log;
} os_table_t;

typedef struct OS_ABI {
    int32_t (*set)(uint32_t pin, uint32_t value);
} os_gpio_t;

typedef struct OS_ABI {
    void (*sleep_ms)(uint32_t ms);
} os_time_t;

typedef struct OS_ABI {
    void (*print)(const char* message);
} os_log_t;
```

MVP rationale:

- `gpio.set()` is required to control the LED.
- `time.sleep_ms()` keeps blink timing out of busy loops.
- `log.print()` gives the payload a simple debug path.
- No memory module is exposed.
- No networking, storage, sensor, or shell ABI is exposed to payloads yet.

## 8. Failure Behavior

Payload failures:

- If AOT load fails, mark the payload as `failed` and keep WebOS running.
- If ABI validation fails, reject the payload before calling its entry point.
- If the payload traps, destroy the runtime instance and keep WebOS running.
- If the payload exceeds its execution budget, yield first; if it remains unhealthy, stop it.
- If the payload cannot be stopped cleanly, destroy the runtime instance and report failure.

HTTP failures:

- `/push` must reject writes during USB storage mode.
- `/ota` must reject updates during USB storage mode.
- `/shell` may run any trusted command for MVP, but WebOS should still apply a timeout so a hung command does not block the server forever.

Filesystem failures:

- If FatFS mount fails in normal mode, start only a minimal recovery server if possible.
- If USB storage mode is active, WebOS must not mount FatFS for internal writes.
- If a file operation fails, return an explicit error to the host tool and leave the current running payload unchanged.

OTA failures:

- If upload fails, delete the staged image.
- If checksum validation fails, reject the staged image.
- If apply fails, keep the current firmware.
- If the new firmware fails to boot, rely on MCUboot rollback if available.

## 9. Strict Four-Phase MVP Roadmap

### Phase 1: Naked Core

Integrate WAMR `iwasm` into Zephyr for ESP32-S3. Hardcode an `add_wasm[]` or `add_aot[]` byte array in flash, call `add(5, 7)`, and print `12` over serial.

### Phase 2: The Bridge

Define `os_table_t` with one syscall: `gpio_set`. Write a C-based WASM payload that receives the table and blinks an LED. This LED blink payload is the first real application and the first end-to-end proof of the ABI.

### Phase 3: The Wire

Add a basic Zephyr HTTP server for host-side tooling. Implement:

- `POST /ota` to upload and apply a new WebOS firmware image.
- `POST /push` with `{ file, path }` to copy a file from the host into FatFS at the requested path.
- `POST /shell` with `{ cmd }` to execute a shell command on the device.

The HTTP server is the primary deployment and management interface when the device is running normally. USB mass-storage mode is for direct filesystem access from macOS and is mutually exclusive with normal runtime behavior.

For MVP, `/shell` is trusted and may execute any command submitted by the owner or development tool. Command whitelisting, authentication, authorization, and audit logging are deferred until the deployment loop is proven.

### Phase 4: Edge OS

Build a small web UI, compress static assets, and serve them from the device filesystem. Support uploading and running AOT payloads produced by the host-side toolchain.

### Later ABI Expansion

After the LED blink MVP works, expand the stable ABI surface gradually. The long-term goal is to support all major device services, including timers, logging, networking, storage, and sensor I/O. This is intentionally not a near-term focus; it should happen only after the core runtime, AOT loading, filesystem behavior, and deployment loop are proven.

## 10. Core References

- WASI ABI specification for standard host-module interfaces.
- WAMR `iwasm` documentation for embedded runtime integration and AOT execution.
- Toit VM memory model for ESP32-oriented memory design ideas.

## 11. Deferred Questions

- Tune WAMR/iwasm AOT compiler flags for ESP32-S3 after the default build path works.
