# WebOS Improvement Plan

## Goal

Turn WebOS from a working WASM payload launcher into a distinctive local-first hardware application runtime.

The product should make this workflow safe and observable:

```text
build application
    -> validate its hardware contract
    -> install it transactionally
    -> run it under supervision
    -> record its hardware interactions
    -> recover or replay when it fails
```

The work is intentionally focused on five connected improvements:

1. Freeze a small ABI v1.
2. Build the application supervisor.
3. Introduce a transactional package format.
4. Make hardware self-describing.
5. Build the hardware time machine.

These features should be implemented in this order. The ABI is the contract used by the supervisor, packages declare the contract and resources they need, hardware metadata resolves those requirements, and tracing observes the resulting capability operations.

## Implementation Progress

- **2026-08-08 - Outbound HTTP extension foundation:** Added a binary-safe WASM `web_http_request()` import with bounded URLs, headers, request/response bodies, and timeouts; HTTP/HTTPS transport; TLS peer and hostname verification; stable extension error codes; and a C Sudoku HTTPS sample. Firmware and sample builds, physical-device flash, and startup/log validation are recorded with the delivering commit.
- **2026-08-08 - ABI v1 foundation:** Defined the canonical ABI 1.0 schema and specification, generated C and Rust core bindings, added immutable pre-instantiation ABI metadata checks, stable errors, binary-safe logging, execution contexts, scoped path/origin policy helpers, readiness and heartbeat signals, health version reporting, C/Rust conformance samples, and focused version/memory/path/policy tests.
- **Still required before Milestone 1 completion:** Derive contexts and grants from installed package manifests instead of the transitional broad shell-development context, move WAMR execution under the supervisor, expand the HTTPS trust store/configuration, enforce an end-to-end DNS/connect/TLS/response deadline, and exercise malformed calls at the real WAMR boundary. The HTTP extension remains outside the ABI 1.0 core.
- **Validation:** Generated bindings were checked; all changed C and Rust samples built with ABI metadata; the focused ABI suite passed 5/5 on QEMU RISC-V; ESP32-S3 firmware built and flashed with `wdb flash`; `/health` reported ABI 1.0; `wdb logcat` reported `Startup: OK`; C and Rust conformance payloads ran; and a legacy payload without ABI metadata was rejected before instantiation.

## Design Principles

- Keep the host firmware stable while applications remain disposable.
- Keep the native ABI small, versioned, binary-safe, and hardware-neutral.
- Never overwrite the active application in place.
- Keep management, networking, OTA, and recovery responsive when an application fails.
- Describe hardware in machine-readable form instead of relying on board-specific documentation.
- Capture application behavior at logical capability boundaries, not inside individual drivers.
- Keep deployment, operation, debugging, and recovery local-first.

## Step 1: Freeze A Small ABI v1

### Objective

Create a stable application contract before adding more host functions. Applications should depend on a small WebOS ABI rather than Zephyr APIs, physical driver details, or incidental runtime behavior.

### ABI Surface

The first stable ABI should contain only the primitives needed to run, observe, and control an application:

```c
void webos_log(unsigned int level, const void *data, unsigned int length);
void webos_sleep_ms(unsigned int milliseconds);
int webos_read(const char *path, void *data, unsigned int capacity);
int webos_write(const char *path, const void *data, unsigned int length);
void webos_ready(void);
void webos_heartbeat(void);
```

Outbound HTTP can remain available as an optional, capability-gated extension. It must not become an unrestricted assumption of every application.

### Rules

- Assign the ABI an explicit semantic version, starting at `1.0`.
- Use explicit pointer and length arguments for binary data.
- Validate every application memory range before native access.
- Keep hardware-specific functions out of the ABI.
- Route application file operations through an application-aware policy layer.
- Define stable error codes independent of Zephyr's internal error values where practical.
- Make compatible changes append-only; breaking changes require a new ABI major version.
- Expose runtime limits and optional extensions so applications can detect them.
- Generate the C header and Rust declarations from one interface definition when possible.

### Work Items

1. Write an ABI specification covering types, memory ownership, errors, threading, path encoding, and compatibility.
2. Rename or wrap the current imports so the public v1 names are consistent.
3. Add lifecycle signals for readiness and heartbeat reporting.
4. Add an application execution context to every host call.
5. Gate filesystem and HTTP access using that context.
6. Add malformed-pointer, oversized-buffer, invalid-path, and unsupported-version tests.
7. Add ABI conformance samples in C and Rust.
8. Record the ABI version in firmware health information and package manifests.

### Deliverables

- `doc/abi-v1.md`
- A canonical ABI definition and generated SDK bindings
- C and Rust conformance applications
- Host-side and Zephyr tests for ABI validation

### Acceptance Criteria

- A v1 application runs unchanged after a compatible host firmware update.
- Unsupported ABI versions are rejected before application execution.
- Invalid application pointers cannot be dereferenced by the host.
- Applications cannot access hardware or storage outside their granted paths.
- The ABI can be documented completely without referring to Zephyr driver APIs.

## Step 2: Build The Application Supervisor

### Objective

Move WAMR execution out of synchronous shell handling and into a dedicated service that owns the complete application lifecycle.

### State Machine

```text
uninstalled
    |
    v
installed -> starting -> probation -> running -> stopping -> stopped
                |           |            |
                +-----------+------------+
                            v
                          failed -> backoff -> starting
                            |
                            v
                         disabled
```

Only one application needs to run at a time for the first implementation. The design should avoid assumptions that make multiple applications impossible later.

### Supervisor Responsibilities

- Own the WAMR module, instance, execution environment, arguments, and output sink.
- Execute applications in a dedicated Zephyr thread.
- Implement install-aware start, stop, status, restart, and autostart operations.
- Enforce stack, linear memory, runtime, and restart limits.
- Interrupt or terminate applications that cannot stop cooperatively.
- Detect traps, abnormal exits, readiness failures, and missed heartbeats.
- Apply bounded exponential restart backoff.
- Persist the active application and its desired state.
- Keep WebOS management services responsive during execution and failure handling.
- Emit structured lifecycle events for logs, status, and tracing.

### Persistent Record

A minimal persistent application record should include:

```json
{
  "id": "room-light",
  "active_version": "1.2.0",
  "previous_version": "1.1.0",
  "desired_state": "running",
  "observed_state": "failed",
  "restart_count": 3,
  "last_exit_reason": "heartbeat-timeout",
  "last_transition_ms": 18422
}
```

Writes to this record must be recoverable after reset or power loss. Volatile runtime details can be reconstructed at boot.

### Work Items

1. Separate WAMR initialization from module execution.
2. Introduce an application manager service with a command queue and event stream.
3. Move module execution into a dedicated Zephyr thread.
4. Remove global shell ownership such as the active shell output pointer.
5. Add application-scoped logging with application ID and version.
6. Implement `start`, `stop`, `status`, and `restart` service operations.
7. Add readiness deadlines and heartbeat monitoring.
8. Add restart budgets and bounded exponential backoff.
9. Persist desired state and autostart it after local services initialize.
10. Expose typed HTTP operations and matching `wdb app` commands.

### Initial Commands

```sh
wdb app list
wdb app status room-light
wdb app start room-light
wdb app stop room-light
wdb app restart room-light
wdb app logs room-light --follow
```

### Acceptance Criteria

- An infinite-loop test application can be stopped within a defined deadline.
- A trapping application does not crash or block the host firmware.
- HTTP, OTA, status, and logs remain available while an application runs.
- Repeated crashes end in a stable `disabled` state instead of a restart loop.
- An autostart application resumes after reboot.
- Every state transition includes a timestamp and reason.

## Step 3: Introduce A Transactional Package Format

### Objective

Replace unmanaged raw `.wasm` uploads with versioned `.webpkg` packages that are validated, installed atomically, and safe to roll back.

### Package Layout

Use a deterministic archive with a small, inspectable structure:

```text
manifest.json
app.wasm
assets/
signature.ed25519
```

Example manifest:

```json
{
  "schema": 1,
  "id": "room-light",
  "version": "1.2.0",
  "abi": "1.0",
  "entrypoint": "app.wasm",
  "resources": {
    "memory_kib": 256,
    "stack_kib": 32,
    "ready_timeout_ms": 5000,
    "heartbeat_ms": 5000
  },
  "capabilities": {
    "led": ["status"],
    "button": ["action"],
    "network": ["https://api.example.com"]
  },
  "files": {
    "app.wasm": "sha256:..."
  }
}
```

### Installation Transaction

1. Stream the upload into a uniquely named staging directory.
2. Reject size, path, nesting, and file-count limit violations while streaming.
3. Parse and validate the manifest schema.
4. Verify the hash of every package file.
5. Verify the application ID, version, ABI, architecture, and resource limits.
6. Resolve and authorize requested hardware and network capabilities.
7. Verify the owner signature when signature enforcement is enabled.
8. Close and synchronize all staged files.
9. Atomically rename the staged directory into the version store.
10. Persist a journal entry selecting the new candidate version.
11. Start the candidate in supervisor probation.
12. Mark it active only after readiness and stability checks pass.
13. Preserve the former active version as the rollback candidate.

Never overwrite the active application's files in place.

### Suggested Storage Layout

```text
/STORAGE:/apps/
  room-light/
    versions/
      1.1.0/
      1.2.0/
    state.json
    transaction.log
/STORAGE:/staging/
/STORAGE:/apps-data/
  room-light/
```

### Recovery Rules

- An incomplete staging directory is never executable.
- A package renamed into the version store is immutable.
- Boot recovery replays or rolls back incomplete journal operations.
- A failed candidate returns control to the previous healthy version.
- Application data survives normal upgrades and rollback.
- Uninstall requires an explicit choice to preserve or delete application data.

### Work Items

1. Specify `.webpkg` archive normalization and manifest schema.
2. Implement a streaming installer that does not buffer the entire package in RAM.
3. Add hash, ABI, architecture, resource, and capability validation.
4. Add a persistent transaction journal.
5. Integrate candidate activation with supervisor probation.
6. Add explicit activate, rollback, and uninstall operations.
7. Add `wdb app build`, `inspect`, `install`, and `rollback` commands.
8. Add fault-injection tests at every transaction boundary.

### Acceptance Criteria

- Resetting during any installation stage leaves either the previous valid version or the complete new version available.
- A corrupt, oversized, incompatible, or unauthorized package never becomes active.
- A new version that fails readiness automatically rolls back.
- `wdb app inspect` reports compatibility before upload.
- Installing a package does not require rebuilding or reflashing host firmware.

## Step 4: Make Hardware Self-Describing

### Objective

Turn devfs from a useful path convention into a machine-readable hardware contract used by applications, `wdb`, tests, and generated user interfaces.

### Device Metadata

Each devfs node should register metadata alongside its file operations:

```json
{
  "id": "status-light",
  "type": "rgb-color",
  "path": "/dev/led/48/color",
  "access": "write",
  "encoding": "rgb8",
  "constraints": {
    "length": 3
  }
}
```

Common metadata fields should include:

- Stable logical identifier
- Device class and value type
- Physical devfs path
- Read, write, or event access
- Encoding and byte order
- Unit and scale
- Minimum, maximum, and step
- Enumeration values
- Update frequency or event behavior
- Driver and board identity
- Human-readable label and description

### Logical Capability View

Keep physical resources under `/dev`, but expose application-specific logical paths:

```text
Host:        /dev/led/48/color
Application: /cap/status-light/color
```

A device mapping resolves package requirements to physical hardware:

```json
{
  "status-light": "/dev/led/48",
  "action-button": "/dev/gpio/2"
}
```

Applications should not receive unrestricted direct access to `/dev`.

### Hardware Passport

Expose a generated hardware passport through a typed management endpoint:

```json
{
  "board": "webos-esp32s3",
  "abi": "1.0",
  "limits": {
    "wasm_memory_kib": 1024,
    "package_bytes": 524288
  },
  "devices": [],
  "extensions": ["trace-v1", "http-client-v1"]
}
```

### Work Items

1. Define metadata structures and registration APIs in devfs.
2. Add metadata to GPIO and RGB LED wrappers.
3. Generate a stable hardware passport from registered nodes.
4. Add an application-specific `/cap` path resolver.
5. Add package capability matching and permission checks.
6. Expose the passport through HTTP and `wdb hardware describe`.
7. Add `wdb app inspect --device` compatibility reporting.
8. Generate basic controls from metadata for validation and demos.

### Acceptance Criteria

- `wdb hardware describe` reports available devices without board-specific parsing.
- A package with unresolved capabilities is rejected before activation.
- The same package runs unchanged on two boards with different physical mappings.
- Applications cannot open undeclared physical hardware paths.
- New driver metadata automatically appears in the hardware passport.

## Step 5: Build The Hardware Time Machine

### Objective

Record application interactions with logical hardware and replay them to reproduce physical-device behavior without requiring the original event sequence or device.

This is the primary creative differentiator. It turns the capability layer into an observable boundary for debugging, regression testing, and demonstrations.

### Trace Model

Capture operations after logical capability resolution but before or after the physical driver call as appropriate:

```json
{"t_us":0,"app":"room-light","version":"1.2.0","op":"read","path":"/cap/action-button/value","rc":1,"value":"0"}
{"t_us":812000,"app":"room-light","version":"1.2.0","op":"read","path":"/cap/action-button/value","rc":1,"value":"1"}
{"t_us":814000,"app":"room-light","version":"1.2.0","op":"write","path":"/cap/status-light/color","rc":3,"value":"ff8000"}
```

A trace header should include:

- Trace format version
- Device and board identity
- Firmware and ABI versions
- Application ID, version, and package hash
- Capability mapping snapshot
- Start time, clock source, and time resolution
- Redaction and payload-capture policy

### Capture Scope

Implement tracing incrementally:

1. Application lifecycle transitions
2. Capability reads and writes
3. Sleep and timer operations
4. Traps, exits, readiness, and heartbeat events
5. External events such as buttons and sensors
6. Optional network request metadata and replayable responses

Secrets, authorization headers, credentials, and private application data must be excluded or redacted by default.

### Replay Modes

#### Device Replay

Inject recorded input reads and events while running the application on a device. Compare actual output writes against the trace.

#### Host Replay

Run the package with a host WAMR runtime and virtual capability filesystem. Supply recorded inputs and capture outputs without physical hardware.

#### Regression Replay

Run the same trace against two application versions and report behavioral differences:

```text
expected: /cap/status-light/color = ff8000 at 814 ms
actual:   /cap/status-light/color = ff0000 at 814 ms
```

### Commands

```sh
wdb trace record room-light
wdb trace stop
wdb trace list
wdb trace inspect run.wtrace
wdb trace replay run.wtrace
wdb trace diff good.wtrace candidate.wtrace
```

### Storage And Performance

- Use a bounded binary trace format on the device.
- Stream traces to `wdb` when connected.
- Fall back to a ring buffer when disconnected.
- Record monotonic timestamp deltas instead of full timestamps per event.
- Make payload capture configurable by capability and data sensitivity.
- Report dropped events explicitly rather than silently producing an incomplete trace.
- Keep tracing optional and measure its memory, storage, and timing overhead.

### Work Items

1. Define the `.wtrace` format and redaction rules.
2. Add a lightweight trace event API to the supervisor and capability layer.
3. Record lifecycle events and devfs reads/writes first.
4. Implement a bounded on-device trace ring buffer.
5. Add trace download and inspection to `wdb`.
6. Implement device replay with virtualized capability inputs.
7. Build a host WAMR replay runner.
8. Add deterministic clock and timer handling during replay.
9. Add trace comparison with useful mismatch diagnostics.
10. Promote saved traces into repeatable integration tests.

### Acceptance Criteria

- A button and LED interaction can be recorded on a physical device.
- The trace identifies the exact package and hardware mapping used.
- The recorded interaction can be replayed without manually pressing the button.
- A changed application version produces a clear output diff.
- Trace capture does not make management services unresponsive.
- Sensitive headers and application data are not captured by default.

## Integrated Delivery Sequence

### Milestone 1: Managed Execution

Implement ABI v1 and the application supervisor.

Demo:

```sh
wdb app start hello
wdb app status hello
wdb app stop hello
```

Exit condition: traps and infinite loops are contained while WebOS remains responsive.

### Milestone 2: Safe Deployment

Implement `.webpkg`, staging, activation probation, and rollback.

Demo:

```sh
wdb app install room-light-1.1.0.webpkg
wdb app install room-light-1.2.0-broken.webpkg
# The candidate fails readiness and WebOS restores 1.1.0.
```

Exit condition: fault-injection tests prove installation is recoverable at every transaction boundary.

### Milestone 3: Portable Hardware Contracts

Add devfs metadata, hardware passports, capability matching, and `/cap` views.

Demo: install the same package on two boards whose status LEDs use different physical pins.

Exit condition: no application source or package change is required between boards.

### Milestone 4: Hardware Time Machine

Add trace recording, inspection, replay, and comparison.

Demo:

```sh
wdb trace record room-light
# Reproduce a physical input sequence.
wdb trace replay room-light-bug.wtrace
wdb trace diff room-light-bug.wtrace room-light-fixed.wtrace
```

Exit condition: a hardware behavior bug recorded on-device is reproduced and verified against a fixed version without repeating the physical interaction.

## Testing Strategy

Each milestone should add tests at three levels:

- **Unit tests:** manifest parsing, ABI validation, state transitions, metadata matching, and trace codecs.
- **Zephyr integration tests:** supervisor behavior, devfs policy, filesystem recovery, and bounded trace storage.
- **Physical-device tests:** traps, stop deadlines, reset during installation, cross-board mapping, and trace/replay workflows through `wdb`.

Required failure cases include:

- Invalid WASM pointers and lengths
- Unsupported ABI and package schema versions
- Out-of-memory during load and execution
- Trap before and after readiness
- Missed heartbeat and infinite loop
- Reset during every installation transaction step
- Missing or conflicting capabilities
- Full storage and truncated package upload
- Trace ring-buffer overflow
- Replay input exhaustion and output mismatch

## Out Of Scope Until These Milestones Are Stable

- Multiple concurrent applications
- Public application marketplace
- Cloud fleet management
- A large device dashboard
- Broad peripheral coverage without metadata contracts
- USB drop-to-deploy
- Unrestricted application networking

These can follow later, but they should not delay the core promise: portable hardware applications that install safely, run under supervision, describe their requirements, and can be replayed when behavior goes wrong.
