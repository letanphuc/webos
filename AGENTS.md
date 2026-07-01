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
- The target board is `esp32s3_devkitc/esp32s3/procpu` (was `esp32s3_devkitm`; Zephyr maps the deprecated name automatically).
- After `source .env`, use `build`, `rebuild`, `flash`, `run`, `monitor`, `menuconfig`, `clean`, and `twister_app` from the workspace root.
- Standard device workflow from the workspace root:
  - Incremental build: `source .env && build`
  - Clean rebuild after Zephyr/MCUboot/config branch changes: `source .env && rebuild`
  - Flash current build: `source .env && flash`
  - Build and flash: `source .env && run`
  - Interactive UART logs: `source .env && monitor`
  - Timed UART logs: `gtimeout 20s script -q /dev/null bash -c 'source .env && WEBOS_PORT=/dev/tty.usbserial-130 monitor'`
  - Build with explicit device port: `source .env && WEBOS_PORT=/dev/tty.usbserial-130 flash`
- `monitor` runs `west espressif monitor` using `WEBOS_PORT`, defaulting to `/dev/tty.usbserial-1130` at `115200` baud. The flash helper uses `WEBOS_BAUD=460800`; the current frequently used device is `/dev/tty.usbserial-130`.
- If flashing fails with `Resource busy`, close any monitor session and check both serial aliases with `lsof /dev/tty.usbserial-130 /dev/cu.usbserial-130`, then retry `source .env && WEBOS_PORT=/dev/tty.usbserial-130 flash`.
- For device-side HTTP, shell, file push, log, OTA, and WASM smoke tests, use `webdb` from the workspace root after `source .env`, e.g. `tools/webdb/target/debug/webdb shell fs ls /dev` or `tools/webdb/target/debug/webdb shell iwasm exec /STORAGE:/apps/blink2.wasm 2`.
- Avoid raw `curl` and custom Python snippets for device HTTP/shell testing; use `tools/webdb/target/debug/webdb ...` so host/device interactions follow the repo-supported path.
- Common `webdb` checks after Wi-Fi connects:
  - Health/shell smoke: `source .env && tools/webdb/target/debug/webdb shell fs ls /dev`
  - Read logs: `source .env && tools/webdb/target/debug/webdb log`
  - Follow logs: `source .env && tools/webdb/target/debug/webdb log --follow`
  - OTA upload: `source .env && tools/webdb/target/debug/webdb ota build/app/zephyr/zephyr.signed.bin`
- WiFi credentials live in `app/wifi.conf` (gitignored). `build` and `rebuild` load it via `EXTRA_CONF_FILE`. Override with `WEBOS_WIFI_SSID`/`WEBOS_WIFI_PSK` env vars.
- For extra config fragments (e.g. debug): `build -- -DEXTRA_CONF_FILE=debug.conf`. Multiple fragments: `build -- -DEXTRA_CONF_FILE="debug.conf;other.conf"`.
- Run application Twister builds after `source .env` with `twister_app`.
- Run library tests with `west twister -T webos/tests -v --inline-logs --integration` from the workspace root.

## Boot And OTA Debugging

- Normal non-RAM-load boot shows MCUboot loading the primary image from flash and mapping IROM/DROM from flash:
  - `I (boot): Loading image 0 - slot 0 from flash, area id: 2`
  - `I (boot): IROM ... map`
  - `I (boot): DROM ... map`
- RAM-load boot shows MCUboot copying from `slot0` into PSRAM before validation and jump:
  - `MCUboot PSRAM mapped at vaddr=0x3c000000 size=0x800000`
  - `flash_esp32: PSRAM flash read req src=0x20000 dst=0x3c000000 len=...`
  - `mcuboot: Image 0 loaded from the primary slot`
  - `mcuboot: booting RAM-loaded ESP32 image at ...`
- If the app is reverted to non-RAM-load but Zephyr or MCUboot remains on RAM-load commits, boot can fail or MCUboot can try to erase/remove a secondary image. Keep app, Zephyr, and MCUboot commits aligned before `run`.
- Known stable non-RAM-load alignment from the RAM-load rollback session:
  - app repo `main` at or after `7842c1e Revert "app: enable ESP32-S3 MCUboot RAM load"`
  - Zephyr at `d4edf519ff9` (parent of local RAM-load patch `aaa5e9b5ed4`)
  - MCUboot at `0fae8920`
- RAM-load branches/commits kept for reference:
  - app `origin/ram-load`
  - Zephyr local commit `aaa5e9b5ed4 esp32s3: support MCUboot PSRAM RAM load`
  - MCUboot fork `git@github.com:letanphuc/zephyr-mcuboot.git`, branch `ram-load`, commit `9efb22a8ec6ea39950fa5e26ce8a4bf7f275642f`
- RAM-load OTA findings are documented in `docs/ram-load-ota.md`. Important lesson: live erase/write of `slot0` while the full app and Wi-Fi stack are running is not proven safe. Direct `HTTP -> slot0`, 4 KB inline buffers, double-buffer worker flashing, and erase-up-front all showed stalls, timeouts, or fatal exceptions. Full-image PSRAM staging completed once but is not proven reliable.
- For future fast OTA work, prefer a staging/updater design over live `slot0` mutation. Do not assume RAM-load means all runtime code, rodata, Wi-Fi blobs, interrupt paths, and flash/cache operations are independent of the original flash slot.

## Coding Style

- C/C++ source formatting is defined by `webos/.clang-format` using Google style with `ColumnLimit: 120`.
- After `source .env` from the workspace root, run `formatcode` to format all C/C++ source and header files under `webos/`.
- Always run `formatcode` before committing code changes. If formatting changes files, include those formatted files in the same commit as the code change.
- `formatcode` is defined in `webos/app/.env`; keep `/Users/phuc/Work/webos/.env` and `webos/app/.env` aligned if helper behavior changes.

## Current Zephyr Wiring

- `zephyr/module.yml` makes this repository a Zephyr module and sets `board_root: .` and `dts_root: .`; custom boards and DTS bindings under this repo are visible to Zephyr builds.
- Root `CMakeLists.txt` is the module entry point. It adds `drivers/` and `lib/`, not the application entry point.
- `/Users/phuc/Work/webos/.env` is outside this git repo but is part of the local workspace contract; keep it aligned with `app/.env` if helper behavior changes.

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
- The flash disk is currently RAM-backed (`CONFIG_DISK_DRIVER_RAM=y`); contents are lost on reboot. Switch to real flash partition for persistent storage.
