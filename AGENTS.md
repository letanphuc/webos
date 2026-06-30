# AGENTS.md

## Repository Role

- This repo is a Zephyr workspace application, not the Zephyr tree itself. The local manifest is `west.yml`; the app repo lives at `webos/` inside the west workspace root.
- OpenCode is intended to start from `/Users/phuc/Work/webos`; its config is `/Users/phuc/Work/webos/opencode.json` and loads this file through `webos/AGENTS.md`.
- Treat `docs/idea.md` as the product direction for WebOS: Zephyr + WAMR `iwasm`, ESP32-S3 with PSRAM, FatFS, host-side AOT, trusted MVP payloads.
- Keep `docs/idea.md` when replacing or pruning example-application files.

## Workspace Commands

- From the west workspace root `/Users/phuc/Work/webos`: `west init -l webos` then `west update`.
- Use the workspace-root env first: `cd /Users/phuc/Work/webos && source .env`. It activates `/Users/phuc/Work/zephyr/.venv`, sets `WEBOS_APP_DIR=/Users/phuc/Work/webos/webos/app`, sets `WEBOS_BUILD_DIR=/Users/phuc/Work/webos/build`, and loads `webos/app/.env`.
- The manifest allowlist currently pulls only `zephyr`, `hal_espressif`, `mbedtls`, and `mcuboot`; add modules in `west.yml` before using other Zephyr subsystems that require external modules.
- The target board is the app-provided `webos_esp32s3/esp32s3/procpu`.
- After `source .env`, use `build`, `rebuild`, `flash`, `run`, `monitor`, `menuconfig`, `clean`, and `twister_app` from the workspace root.
- Standard device workflow from the workspace root:
  - Build: `source .env && build`
  - Flash: `source .env && flash`
  - Build and flash: `source .env && run`
  - View logs interactively: `source .env && monitor`
  - View logs with timeout: `gtimeout 10s script -q /dev/null bash -c 'source .env && monitor'`
- `monitor` runs `west espressif monitor` using `WEBOS_PORT`, defaulting to `/dev/tty.usbserial-1130` at `115200` baud. The flash helper uses `WEBOS_BAUD=460800`.
- For device-side HTTP, shell, file push, log, OTA, and WASM smoke tests, use `webdb` from the workspace root after `source .env`, e.g. `tools/webdb/target/debug/webdb shell fs ls /dev` or `tools/webdb/target/debug/webdb shell iwasm exec /STORAGE:/apps/blink2.wasm 2`.
- Avoid raw `curl` and custom Python snippets for device HTTP/shell testing; use `tools/webdb/target/debug/webdb ...` so host/device interactions follow the repo-supported path.
- WiFi credentials live in `app/wifi.conf` (gitignored). `build` and `rebuild` load it via `EXTRA_CONF_FILE`. Override with `WEBOS_WIFI_SSID`/`WEBOS_WIFI_PSK` env vars.
- For extra config fragments (e.g. debug): `build -- -DEXTRA_CONF_FILE=debug.conf`. Multiple fragments: `build -- -DEXTRA_CONF_FILE="debug.conf;other.conf"`.
- Run application Twister builds after `source .env` with `twister_app`.
- Run library tests with `west twister -T webos/tests -v --inline-logs --integration` from the workspace root.

## Coding Style

- C/C++ source formatting is defined by `webos/.clang-format` using Google style with `ColumnLimit: 120`.
- After `source .env` from the workspace root, run `formatcode` to format all C/C++ source and header files under `webos/`.
- Always run `formatcode` before committing code changes. If formatting changes files, include those formatted files in the same commit as the code change.
- `formatcode` is defined in `webos/app/.env`; keep `/Users/phuc/Work/webos/.env` and `webos/app/.env` aligned if helper behavior changes.

## Current Zephyr Wiring

- `zephyr/module.yml` makes this repository a Zephyr module and sets `board_root: .` and `dts_root: .`; custom boards and DTS bindings under this repo are visible to Zephyr builds.
- Root `CMakeLists.txt` is the module entry point. It adds `drivers/` and `lib/`, not the application entry point.
- `/Users/phuc/Work/webos/.env` is outside this git repo but is part of the local workspace contract; keep it aligned with `app/.env` if helper behavior changes.
- Sysbuild enables MCUboot RAM-load for the app: `app/sysbuild.conf` sets `SB_CONFIG_MCUBOOT_MODE_RAM_LOAD=y` and `SB_CONFIG_MCUBOOT_RAMLOAD_ALLOW_XIP=y`.
- The app and MCUboot overlays both define `mcuboot,image-ram = &psram_exec` at `0x42000000` with a 6 MB window, plus a retained `zephyr,bootloader-info` region at `0x3fcff000`.
- MCUboot for this workspace requires local changes in the west-managed `zephyr/` and `bootloader/mcuboot/` repositories:
  - `zephyr` commit `aaa5e9b5ed4` (`esp32s3: support MCUboot PSRAM RAM load`) initializes octal PSRAM in MCUboot, provides MCUboot PSRAM linker symbols, redirects ESP32-S3 PSRAM flash reads through an internal bounce buffer, and uses `esp_flash_read()` after PSRAM/MSPI init.
  - `bootloader/mcuboot` commit `9efb22a8` (`boot: support ESP32-S3 PSRAM RAM-load handoff`) validates RAM-loaded images from the PSRAM DROM alias, reads TLVs from flash in RAM-load mode, and stages the ESP image from PSRAM before copying final IRAM/DRAM/IROM/DROM segments and jumping to the ESP load-header entry.
- Verified RAM-load boot path: MCUboot initializes PSRAM, copies the signed image from slot 0 to PSRAM, validates the SHA256 TLV, stages/copies ESP segments, jumps to `__start`, and WebOS boots with filesystem, Wi-Fi, devfs, iwasm, and HTTP server startup.

## Application Source Layout

The application source lives under `app/src/` and is organised in four layers:

```
app/src/
├── main.c                              # Entry point: init sequence only
├── hal/
│   └── wifi/
│       ├── wifi.h / wifi.c             # Wi-Fi STA connect with retry + DHCP
├── utils/
│   └── json/
│       ├── json.h / json.c             # JSON string parsing and escaping
└── services/
    ├── fs/
    │   ├── fs.h / fs.c                 # FatFS mount, directory layout, file I/O
    ├── ota/
    │   ├── ota.h / ota.c               # OTA state machine (flash_img + MCUboot)
    ├── http/
    │   ├── http.h / http.c             # Server config, response helpers, SERVICE_DEFINE
    │   ├── http_handlers.h
    │   └── http_handlers.c             # 5 endpoint handlers + RESOURCE_DEFINE macros
    └── iwasm/
        ├── iwasm.h / iwasm.c           # WAMR runtime init, native ABI registration, exec
```

Library code lives under `lib/` and is registered via `lib/CMakeLists.txt`:

```
lib/
├── CMakeLists.txt                      # add_subdirectory_ifdef for each lib
├── Kconfig                             # menu "Custom libraries"
├── custom/                             # Example custom library
├── devfs/                              # Generic virtual /dev filesystem
│   ├── CMakeLists.txt
│   ├── Kconfig                         # CONFIG_WEBOS_DEVFS
│   ├── devfs.h                         # devfs_register_file() / devfs_unregister_file()
│   └── devfs.c                         # Zephyr VFS backend and registered-file dispatcher
```

Sample WASM payloads live under `sampleapps/`:

```
sampleapps/
├── hello/main.c + Makefile             # Fibonacci WASM (non-WASI, builtin libc)
├── blink/main.c + Makefile             # LED blink via dev_fs_write /dev/gpio/N
└── native_blink/main.c + Makefile      # LED blink via native gpio_set()
```

- **`hal/`** — hardware abstraction layer. Currently `wifi/` owns the Wi-Fi connection sequence.
- **`utils/`** — shared utilities. Currently `json/` provides `json_get_string()` and `append_json_string()`.
- **`services/`** — higher-level services. Each service is a self-contained directory.
  - `fs/` — FatFS on flash disk (192 KB `storage_partition` at 0x3b0000, persistent).
    Mount point `/STORAGE:`, managed by fstab `automount`.
    `init_filesystem_layout()` waits for the mount via `fs_stat` retry, then creates
    `/apps/`, `/config/`, `/logs/`, `/ota/`, `/www/`.
    `write_file()` ensures the parent directory before opening, pre-checks existence
    with `fs_stat` to avoid triggering Zephyr's `LOG_ERR` false positives.
    Long filename support enabled (`CONFIG_FS_FATFS_LFN_MODE_STACK`).
  - `ota/` — clean API (`ota_init/begin/write/finish/abort`) with internal per-call locking; the HTTP handler never touches the flash context or mutex directly.
  - `http/` — `http.c` owns the server definition and JSON/text response helpers; `http_handlers.c` owns all five endpoint handlers (`GET /`, `GET /health`, `POST /push`, `POST /shell`, `POST /ota`) and registers them via iterable linker sections.
  - `iwasm/` — WAMR runtime lifecycle. `iwasm_init()` sets up the PSRAM-backed custom
    allocator and registers native ABI functions (`gpio_set`, `gpio_get`, `sleep_ms`,
    `log_print`, `dev_fs_write`, `dev_fs_read`) via `RuntimeInitArgs.native_symbols`.
    `dev_fs_write`/`dev_fs_read` wrap `fs_open`/`fs_write`/`fs_read` so WASM payloads
    can access `/dev/gpio` files via the Zephyr VFS.
- `app/CMakeLists.txt` lists every `.c` file and adds `target_include_directories(app PRIVATE src)` so that `#include "hal/wifi/wifi.h"`-style paths work from any source file.
- `app/sections-rom.ld` provides the iterable ROM section that binds `HTTP_RESOURCE_DEFINE` entries from `http_handlers.c` to the `HTTP_SERVICE_DEFINE` in `http.c`.
- `app/app.overlay` defines the fstab entry (`zephyr,fstab,fatfs`, automount, disk-access) and the flash disk (`zephyr,flash-disk`) backed by `&storage_partition`.
- `app/app.overlay` also defines the PSRAM RAM-load execution window (`psram_exec@42000000`) and retained MCUboot bootloader-info region (`bootloader_info_mem@3fcff000`). Keep these aligned with `bootloader/mcuboot/boot/zephyr/app.overlay`.
- `lib/devfs/` — generic Zephyr VFS backend registered with `fs_register()` and mounted at `/dev`. It owns path dispatch, file open/read/write/close forwarding, and directory enumeration for registered nodes. It does not know about GPIO or any concrete device class. Device wrappers register files with `devfs_register_file()`; enable with `CONFIG_WEBOS_DEVFS=y`; requires `CONFIG_FILE_SYSTEM_MAX_TYPES=3`.
- `drivers/webos_gpio/` — `webos,gpio` devicetree wrapper around Zephyr GPIO. Each enabled `webos,gpio` node maps to a Zephyr GPIO spec and registers `/dev/gpio/<pin>/value` (rw `"0"`/`"1"`) plus `/dev/gpio/<pin>/direction` (rw `"out"`/`"in"`) through devfs. Enable with `CONFIG_WEBOS_GPIO=y`; the default app overlay maps the first WebOS GPIO to Zephyr `gpio0` pin `2`.

## CI And Docs

- GitHub Actions are still upstream example workflows. Build CI uses `arm-zephyr-eabi`, so it does not yet verify ESP32-S3/Xtensa builds.
- Docs CI builds `doc/` with Doxygen and Sphinx; this is separate from `docs/idea.md`.

## Known Bugs

- RGB LED control is not physically working as expected. The `webos,rgbled` devfs wrapper and host command path can write/read `/dev/rgbled/48/color`, and `led_strip_update_rgb()` returns success, but the onboard RGB LED remains on or shows the wrong color. Tested approaches included custom GPIO bit-banging, Zephyr `worldsemi,ws2812-spi` on `spi2`/`spi3`, GPIO48 as `SPIM3_MOSI_GPIO48`, 7 MHz 8-bit WS2812C frames, 2.4 MHz 3-bit WS2812 frames, and `reset-delay = <300>`. Treat this as unresolved pin-routing/timing/hardware mapping work; do not assume GPIO48 + SPI currently drives the physical LED correctly. Next likely test is the vendor/example data pin GPIO14 or checking the board schematic/logic analyzer trace.

## WebOS MVP Constraints

- MVP target is ESP32-S3 with PSRAM.
- Prefer WAMR/iwasm AOT payloads built by the host-side WAMR AOT compiler using default flags first. (Currently interpreter-only: the prebuilt `wamrc` lacks Xtensa target; build from source with `build_llvm_xtensa.sh` when needed.)
- Do not add an OS memory ABI such as `os->mem`, `os->malloc()`, or `os->mem->alloc_dma()`; payloads should use libc memory APIs inside WASM memory.
- First payload is `/apps/blink.wasm`; native ABI provides `gpio_set`, `gpio_get`, `sleep_ms`, `log_print`, `dev_fs_write`, `dev_fs_read`. Payloads access `/dev/gpio` via the `dev_fs_*` wrappers (which call Zephyr VFS → devfs → GPIO driver).
- WASI (libc-wasi) is compiled in but crashes the device during `wasm_runtime_instantiate` — a Zephyr platform incompatibility in WAMR's SSP layer. Use native imports (registered via `RuntimeInitArgs.native_symbols`) until WASI is fixed.
- FatFS is the planned filesystem. USB mass-storage mode is exclusive: stop payloads and reject filesystem-changing HTTP operations while the host has the volume mounted.
- The flash disk is backed by the real `storage_partition` via `zephyr,flash-disk`; contents are intended to persist across reboot.
