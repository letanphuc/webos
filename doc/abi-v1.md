# WebOS Application ABI v1

## Version and discovery

WebOS ABI 1.0 is imported from the WebAssembly module `env`. An application MUST carry a
four-byte little-endian `(major << 16) | minor` value in the `.custom_section.webos.abi` custom
section. The loader reads this non-executable metadata before module instantiation and rejects a
missing section, a different major, or a minor newer than the host. ABI 1.x changes
are append-only; signatures and meanings below do not change. A breaking change uses a new major.
The firmware `/health` response reports `abi`. Package manifests record the string `abi` and list
optional `extensions`.

`abi/webos-v1.yaml` is normative for names, values, limits, and signatures. Run
`python3 scripts/generate_abi.py` to produce `sdk/c/webos.h` and `sdk/rust/webos.rs`.

## Types and memory

All integers are WebAssembly `i32`; unsigned arguments use their 32-bit bit pattern. Pointers are
offsets in the calling module's linear memory, never host pointers. A buffer is borrowed only for
the duration of a call. Input buffers are read-only; output buffers may be written up to capacity.
The application retains ownership. Zero-length buffers may use a null pointer. Every non-empty
range, including overflow of `pointer + length`, is checked by WAMR before native access. I/O is
limited to 65,536 bytes per call and paths to 255 bytes excluding NUL.

Paths are NUL-terminated UTF-8, absolute logical paths. Empty paths, controls, `//`, trailing `/`,
and `..` components are invalid. Storage is private to `/STORAGE:/apps/<app-id>/data`; `/dev` paths
require explicit grants. Grant matching occurs on path-component boundaries, never simple string
prefixes. Hardware details are represented by logical files, not ABI functions.

## Core functions

- `webos_log(level, data, length)` emits a binary-safe borrowed message at levels 1 error, 2 warn,
  3 info, or 4 debug. Unknown levels are treated as info.
- `webos_sleep_ms(milliseconds)` yields the application thread for at least the requested duration.
- `webos_read(path, data, capacity)` returns bytes read or a stable negative error.
- `webos_write(path, data, length)` returns bytes written or a stable negative error.
- `webos_ready()` idempotently marks the application ready and records monotonic host time.
- `webos_heartbeat()` records the most recent monotonic heartbeat time.

Host calls execute synchronously on the application's execution thread. Calls from different
application instances may run concurrently; applications must not rely on global ordering. The host
owns execution contexts and lifecycle timestamps. Applications cannot pass or inspect a context.

## Stable errors

`0` is success where a status is returned; positive read/write values are byte counts. Stable errors
are `-1 INVALID`, `-2 DENIED`, `-3 NOT_FOUND`, `-4 IO`, `-5 TOO_LARGE`, `-6 BUSY`, and
`-7 UNSUPPORTED`. Zephyr errno values do not cross the ABI boundary. Logging, sleeping, and
lifecycle calls have no result; malformed input is ignored rather than dereferenced.

## Optional extensions and limits

Core availability never implies network access. `webos.http/1` is an optional C and Rust binding that
preserves `web_http_request`. A production package must declare the extension and allowed origins;
every URL is checked against its execution context, and an absent grant returns
`WEB_HTTP_ERR_DENIED`. HTTP-specific compile-time limits currently remain firmware Kconfig values.
Future runtime limit discovery is append-only and requires a new optional extension.

The current shell launcher is explicitly a development context and grants broad device, storage,
and HTTP/HTTPS access because raw `.wasm` execution has no package manifest. This exception must
not be used by the package supervisor; package execution derives the application ID and grants from
the validated manifest.

## Compatibility and lifecycle

Hosts support applications with the same major and a requested minor no greater than the host
minor. Hosts may reduce permissions or configured resource limits, but do not reinterpret calls.
`ready` and `heartbeat` are signals, not barriers; calling either before or after readiness is safe.
The supervisor may use their timestamps for deadlines without exposing Zephyr scheduling APIs.
