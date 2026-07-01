# ESP32-S3 MCUboot RAM-Load OTA Notes

This document records the current WebOS RAM-load implementation, the OTA experiments, and the reliability issues found while trying to make a fast single-slot update path.

## Goal

The target update model is:

1. MCUboot loads the application image from `slot0_partition` into ESP32-S3 PSRAM.
2. The application executes from PSRAM, not directly from the flash slot.
3. OTA writes the next application image into `slot0_partition`.
4. Reboot loads the updated `slot0` image into PSRAM.
5. No MCUboot image swap is performed.
6. `slot1_partition` is not used as a fallback boot source for the WebOS MVP.

The motivation is update speed. Standard MCUboot swap requires writing the uploaded image to the secondary slot, then copying/swapping during reboot. A single-slot RAM-load design should avoid the swap copy and write the final image only once.

## Relevant Commits

Application repository:

- Last pre-RAM-load application commit: `04783524185816affc2afc979d76ad387614277c`.
- RAM-load application branch pushed as: `origin/ram-load` in `git@github.com:letanphuc/webos.git`.

Zephyr repository:

- RAM-load support commit observed locally: `aaa5e9b5ed4 esp32s3: support MCUboot PSRAM RAM load`.
- Push to upstream `zephyrproject-rtos/zephyr` was denied, as expected without upstream permissions.

MCUboot repository:

- RAM-load support commit pushed to fork branch: `9efb22a8ec6ea39950fa5e26ce8a4bf7f275642f` -> `git@github.com:letanphuc/zephyr-mcuboot.git`, branch `ram-load`.
- Requested local baseline checkout after push: `0fae8920c4e5acb792b3fe766c89c668f42be6ee`.

## Partition Layout Observed

The generated Zephyr devicetree showed:

- `slot0_partition`: offset `0x20000`, size `0x150000`.
- `slot1_partition`: offset `0x170000`, size `0x150000`.
- `storage_partition`: offset `0x3b0000`.

The signed application image was approximately 934 KB during testing. Example image metadata from `imgtool dumpinfo`:

- header magic: `0x96f3b83d`.
- load address: `0x42000000`.
- flags: `RAM_LOAD (0x20)`.
- image size: about `0xe3xxx` bytes, depending on build.

## RAM-Load Boot Path

In the RAM-load branch, MCUboot initializes PSRAM, copies the selected image from flash to PSRAM, validates it, then jumps to the ESP32 image entry point.

Representative boot log:

```text
MCUboot PSRAM mapped at vaddr=0x3c000000 size=0x800000
mcuboot: read image 0 slot 0 header rc=0 magic=0x96f3b83d load=0x42000000 hdr=0x20 img=... flags=0x20
flash_esp32: PSRAM flash read req src=0x20000 dst=0x3c000000 len=...
flash_esp32: PSRAM flash read done
mcuboot: ESP32-S3: invalidated DCache for PSRAM copy addr=0x42000000 size=...
mcuboot: Image 0 RAM loading to 0x42000000 is succeeded.
mcuboot: bootutil_img_validate: hash compare OK
mcuboot: Image 0 loaded from the primary slot
mcuboot: br_image_off = 0x20000
mcuboot: ih_load_addr = 0x42000000
mcuboot: ih_flags = 0x20
mcuboot: booting RAM-loaded ESP32 image at 0x4037bd10
```

The application linker metadata still records ESP image load addresses inside `slot0`, while VMAs are the ESP32-S3 external IROM/DROM alias ranges:

```text
.text          VMA 0x42000000  LMA 0x20000  size ...
.flash.rodata VMA 0x3c0a0000  LMA 0xc0000  size ...
```

That is expected for an ESP image, but it matters for debugging because runtime code, ROM, blobs, cache, or flash driver paths may still consult flash-related metadata or require cache stability while the app is running.

## Bootloader Slot Selection Issue

Default MCUboot RAM-load behavior selected the image with the highest version. When `slot0` became invalid during failed OTA, MCUboot loaded the old valid image from `slot1`:

```text
Image in the primary slot is not valid!
Image 0 loaded from the secondary slot
```

That violates the desired WebOS MVP behavior. The intended MVP behavior is primary-only:

- always load `slot0` into PSRAM;
- never silently fall back to `slot1`;
- if `slot0` is invalid, fail loudly instead of booting stale firmware.

A local MCUboot experiment changed RAM-load selection to use `BOOT_SLOT_PRIMARY` and return failure instead of trying another slot after primary validation failure. That local experimental edit was not included in the pushed `9efb22a8` commit unless it is committed separately later.

## Zephyr Upload Slot Issue

`flash_img_init()` is not suitable for this WebOS single-slot RAM-load model without care.

In `CONFIG_MCUBOOT_BOOTLOADER_MODE_RAM_LOAD=y`, Zephyr's image manager path uses bootloader-info to determine the active slot and upload target. On this board, bootloader-info was not populated:

```text
blinfo_mcuboot: MCUboot data load failed, expected magic value: 0x2016, got: 0x0
mcuboot_dfu: Failed to fetch active slot: -5
```

The fallback path can select an unintended upload slot. For WebOS single-slot OTA, the OTA code should explicitly target `slot0_partition`, for example with:

```c
flash_img_init_id(&ota_flash_ctx, PARTITION_ID(slot0_partition));
```

This avoids relying on the broken active-slot discovery path.

## OTA Experiments

### 1. Direct HTTP to `slot0`

Flow:

```text
HTTP request body -> flash_img_buffered_write() -> slot0
```

Observed behavior:

- Host progress often stalled around `31%`, approximately `294912` bytes sent by the host.
- Instrumentation showed host progress was not equal to target flash commit offset because of TCP buffering and backpressure.
- Longer runs could reach `100%` host upload but time out waiting for an HTTP response.
- Failed or interrupted OTA left `slot0` invalid.
- Before primary-only MCUboot experiments, the board then booted old `slot1`.

Conclusion:

Direct streaming into `slot0` while the full app and Wi-Fi stack are active is unreliable.

### 2. Hardcoded `slot1` OTA

Flow:

```text
HTTP request body -> flash_img_buffered_write() -> slot1
```

Observed behavior:

- OTA completed and rebooted successfully once.
- The response was returned:

```text
OTA complete: ... bytes written, rebooting in 2000 ms
```

Rejected reason:

- If MCUboot ever boots from `slot1`, then writing future OTA images to `slot1` corrupts the active image.
- The WebOS MVP wants `slot0` only, not slot fallback behavior.

### 3. Full PSRAM Staging Then Flash `slot0`

Flow:

```text
HTTP request body -> full image buffer in PSRAM -> flash slot0 once after final chunk -> reboot
```

Observed behavior:

- Upload completed quickly to PSRAM.
- `slot0` flash completed once in testing.
- MCUboot rebooted and validated the primary slot successfully.

Representative target log:

```text
webos_ota: OTA flashed rel=0xe3f2c abs=0x103f2c ret=0 final
webos_ota: OTA slot0 flash complete: 933676 bytes
webos_ota: Rebooting to apply OTA image
mcuboot: Image 0 loaded from the primary slot
mcuboot: booting RAM-loaded ESP32 image at 0x4037bd10
```

Important caveat:

- This is not proof of safety.
- It only reduces the dangerous window to the final flash phase.
- During that phase the full WebOS app, HTTP server, Wi-Fi driver, interrupts, and ESP runtime are still alive while `slot0` is erased/written.
- It may have passed once because no flash/cache-sensitive path ran at the wrong time.

### 4. Two 4 KB PSRAM Buffers With Background Flash Worker

Flow:

```text
HTTP fills 4 KB PSRAM buffer A while worker writes 4 KB buffer B to slot0
```

Observed behavior:

- Build passed.
- Device crashed with repeated fatal exception logs during OTA around early flash progress.

Representative log:

```text
webos_ota: OTA flashed rel=0x1d000 abs=0x3d000 len=4096 ret=0
os: ** FATAL EXCEPTION
```

Conclusion:

Concurrent flash writes from a worker while HTTP/Wi-Fi continues running are unsafe on the current target.

### 5. Single 4 KB PSRAM Buffer Inline Flash

Flow:

```text
HTTP data -> 4 KB PSRAM buffer -> flash 4 KB to slot0 inline -> receive more HTTP data
```

Observed behavior:

- Reduced PSRAM usage.
- Still stalled near `31%`.
- Did not solve the root cause because flash erase/write still happens during the HTTP request path.

Representative log:

```text
webos_ota: OTA flashed rel=0x21000 abs=0x41000 len=0 ret=0 time=95 ms
webdb ota ... [====] 14% ...
```

The logged `len=0` was an instrumentation bug caused by printing after the buffer length was reset.

### 6. Erase `slot0` Once Up Front, Then Stream Writes

Flow:

```text
ota_begin() -> flash_area_flatten(slot0) -> HTTP data -> 4 KB writes without progressive erase
```

Configuration experiment:

```text
CONFIG_STREAM_FLASH_ERASE=n
CONFIG_IMG_ERASE_PROGRESSIVELY=n
```

Observed behavior:

- Build passed.
- Device rebooted or crashed while erasing/writing `slot0`.
- This suggests the running system is not fully independent of `slot0` or flash/cache stability.

Conclusion:

Moving erase to the start did not make live single-slot update safe.

## Root-Cause Assessment

The consistent failure pattern is not a simple 1 KB vs 4 KB buffering problem.

The root issue is:

```text
The running WebOS application is not proven independent from slot0 flash/cache behavior while slot0 is erased or written.
```

Even though MCUboot RAM-loads the application into PSRAM, live mutation of `slot0` can still break the running system. Plausible causes include:

- ESP32-S3 flash erase/write disables or disrupts cache while code, rodata, or interrupt paths expect cache availability.
- Wi-Fi blobs or ESP runtime paths execute code or access constants that are flash/cache-sensitive.
- Some code path still executes from flash or references flash-mapped data despite the RAM-load handoff.
- Watchdog starvation during long flash erase/write operations.
- Runtime loader metadata or flash mapping assumptions still point into the original image slot.
- Interrupts can fire during flash operations and touch unsafe code/data.

The board logs also show:

```text
W (soc_init): Unicore bootloader
```

So this is not currently a normal Zephyr SMP app running on both ESP32-S3 cores. However, Wi-Fi/BT firmware, interrupts, ROM code, and vendor blobs can still introduce flash/cache dependencies outside the main application thread.

## Current Practical Conclusion

For the MVP, full PSRAM staging is the fastest tested path that completed once:

```text
HTTP upload -> full PSRAM image -> flash slot0 -> reboot
```

But it should be treated as a best-effort prototype, not a proven reliable OTA strategy.

The safer rule is:

```text
Do not erase or write slot0 while the full WebOS app and Wi-Fi stack are running.
```

## Recommended Reliable Architecture

To keep the no-swap performance goal while avoiding live `slot0` mutation, use a tiny updater/recovery flow:

```text
main app receives OTA image -> staging area -> reboot into updater -> updater writes slot0 once -> reboot into WebOS
```

Benefits:

- No MCUboot slot swap.
- `slot0` is written once by the updater.
- The full WebOS app and Wi-Fi stack are not alive while `slot0` is erased/written.
- The updater can be kept small and audited for IRAM/PSRAM/flash safety.

Costs:

- Requires a staging area large enough for the signed image.
- Current `storage_partition` is only 192 KB, too small for the current 934 KB image.
- Partition layout needs to reserve space for a staging partition or use external storage.

Possible layout direction:

```text
bootloader
slot0: WebOS app, RAM-loaded to PSRAM
updater: tiny recovery/updater image outside slot0
ota_staging: raw signed image storage
storage: WebOS filesystem
```

The updater should:

1. Validate the staged image header and TLV/hash.
2. Erase `slot0`.
3. Write staged image to `slot0`.
4. Reboot.

## Short-Term Recommendation

Keep full PSRAM staging only as a temporary development path if fast iteration is needed. Before flashing `slot0`, reduce risk by stopping as much runtime activity as possible:

- stop iwasm payload execution;
- stop filesystem-changing operations;
- stop accepting new HTTP requests;
- disconnect or stop Wi-Fi if possible;
- minimize logging during flash;
- ensure watchdogs are handled;
- reboot immediately after flashing.

This still does not prove safety, but it reduces the chance that a flash/cache-sensitive path runs while `slot0` is being modified.

## Open Questions

- Which exact instruction/data fetch causes the crash during `slot0` erase?
- Are any app sections still executing from flash after RAM-load?
- Does the crash reproduce with Wi-Fi disabled and a UART-only erase test?
- Does disabling interrupts around critical flash operations help, or does it trigger watchdog failure?
- Can ESP-IDF flash APIs safely erase the app slot while code is executing from PSRAM on ESP32-S3?
- Can the updater be integrated into MCUboot, or should it be a separate image partition?

## Suggested Diagnostics

1. Add a UART shell command that erases a small range of `slot0` without HTTP or Wi-Fi activity.
2. Run the same erase test with Wi-Fi disabled.
3. Capture full fatal exception register dumps, especially PC, EXCCAUSE, EXCVADDR, and backtrace.
4. Verify whether fault PCs fall inside `0x42000000` PSRAM IROM, `0x403xxxxx` internal IRAM, ROM, or unmapped/erased flash aliases.
5. Audit final ELF/map sections for any code or rodata that remains flash-backed at runtime.
6. Test a minimal updater image that erases/writes `slot0` with Wi-Fi and WebOS services absent.
