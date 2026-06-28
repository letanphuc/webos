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
- `run` builds and flashes. `monitor` runs `west espressif monitor` using `WEBOS_PORT`, defaulting to `/dev/tty.usbserial-1130` at `115200` baud.
- WiFi credentials live in `app/wifi.conf` (gitignored). `build` and `rebuild` load it via `EXTRA_CONF_FILE`. Override with `WEBOS_WIFI_SSID`/`WEBOS_WIFI_PSK` env vars.
- For extra config fragments (e.g. debug): `build -- -DEXTRA_CONF_FILE=debug.conf`. Multiple fragments: `build -- -DEXTRA_CONF_FILE="debug.conf;other.conf"`.
- Run application Twister builds after `source .env` with `twister_app`.
- Run library tests with `west twister -T webos/tests -v --inline-logs --integration` from the workspace root.

## Current Zephyr Wiring

- `zephyr/module.yml` makes this repository a Zephyr module and sets `board_root: .` and `dts_root: .`; custom boards and DTS bindings under this repo are visible to Zephyr builds.
- Root `CMakeLists.txt` is the module entry point. It adds `drivers/` and `lib/`, not the application entry point.
- The current application entry point is `app/src/main.c`; it provides Wi-Fi STA with retry, HTTP server (port 8080), JSON RPC shell, FatFS push API, and MCUboot OTA.
- `/Users/phuc/Work/webos/.env` is outside this git repo but is part of the local workspace contract; keep it aligned with `app/.env` if helper behavior changes.

## CI And Docs

- GitHub Actions are still upstream example workflows. Build CI uses `arm-zephyr-eabi`, so it does not yet verify ESP32-S3/Xtensa builds.
- Docs CI builds `doc/` with Doxygen and Sphinx; this is separate from `docs/idea.md`.

## WebOS MVP Constraints

- MVP target is ESP32-S3 with PSRAM.
- Prefer WAMR/iwasm AOT payloads built by the host-side WAMR AOT compiler using default flags first.
- Do not add an OS memory ABI such as `os->mem`, `os->malloc()`, or `os->mem->alloc_dma()`; payloads should use libc memory APIs inside WASM memory.
- First payload is `/apps/blink.aot`; first ABI should stay minimal: GPIO set, sleep milliseconds, and log print.
- FatFS is the planned filesystem. USB mass-storage mode is exclusive: stop payloads and reject filesystem-changing HTTP operations while the host has the volume mounted.
