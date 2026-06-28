# AGENTS.md

## Repository Role

- This repo is a Zephyr workspace application, not the Zephyr tree itself. The local manifest is `west.yml`; the app repo lives at `webos/` inside the west workspace root.
- The current code is still mostly `zephyrproject-rtos/example-application` scaffolding. Treat `docs/idea.md` as the product direction for WebOS: Zephyr + WAMR `iwasm`, ESP32-S3 with PSRAM, FatFS, host-side AOT, trusted MVP payloads.
- Keep `docs/idea.md` when replacing or pruning example-application files.

## Workspace Commands

- From the west workspace root `/Users/phuc/Work/webos`: `west init -l webos` then `west update`.
- Use the existing Zephyr Python environment before ESP32-S3 builds: `source /Users/phuc/Work/zephyr/.venv/bin/activate`.
- The manifest allowlist currently pulls only `zephyr`, `hal_espressif`, `mbedtls`, and `mcuboot`; add modules in `west.yml` before using other Zephyr subsystems that require external modules.
- The requested MVP board is `esp32s3_devkitm/esp32s3/procpu`; current Zephyr maps that deprecated name to `esp32s3_devkitc/esp32s3/procpu`.
- Build the app from the workspace root with `west build -b esp32s3_devkitm/esp32s3/procpu webos/app` or from `webos/app` after sourcing `.env` with `build`.
- For debug config: `west build -b <board> webos/app -- -DEXTRA_CONF_FILE=debug.conf` from the workspace root.
- Run application Twister builds with `west twister -T webos/app -v --inline-logs --integration` from the workspace root.
- Run library tests with `west twister -T webos/tests -v --inline-logs --integration` from the workspace root.

## Current Zephyr Wiring

- `zephyr/module.yml` makes this repository a Zephyr module and sets `board_root: .` and `dts_root: .`; custom boards and DTS bindings under this repo are visible to Zephyr builds.
- Root `CMakeLists.txt` is the module entry point. It adds `drivers/` and `lib/`, not the application entry point.
- The current application entry point is `app/src/main.c`; it is a minimal printk heartbeat app for ESP32-S3 bring-up.
- `app/.env` defines local helper functions: `build`, `rebuild`, `flash`, `run`, `menuconfig`, `clean`, and `twister_app`.

## CI And Docs

- GitHub Actions are still upstream example workflows. Build CI uses `arm-zephyr-eabi`, so it does not yet verify ESP32-S3/Xtensa builds.
- Docs CI builds `doc/` with Doxygen and Sphinx; this is separate from `docs/idea.md`.

## WebOS MVP Constraints

- MVP target is ESP32-S3 with PSRAM.
- Prefer WAMR/iwasm AOT payloads built by the host-side WAMR AOT compiler using default flags first.
- Do not add an OS memory ABI such as `os->mem`, `os->malloc()`, or `os->mem->alloc_dma()`; payloads should use libc memory APIs inside WASM memory.
- First payload is `/apps/blink.aot`; first ABI should stay minimal: GPIO set, sleep milliseconds, and log print.
- FatFS is the planned filesystem. USB mass-storage mode is exclusive: stop payloads and reject filesystem-changing HTTP operations while the host has the volume mounted.
