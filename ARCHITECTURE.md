# Architecture & Development Notes

This document is for developers who want to understand the internals, fork the code,
or contribute upstream fixes to the `esp32-camera` driver.

---

## Dual-Core Data Flow

```
PY260 sensor → GDMA → PSRAM frame buffers (4 × ~512 KB)
                              ↓
                  cam_psram_jpeg_task  (Core 1)
                  - validates SOI/EOI
                  - copies to frame_pool (4 × 512 KB PSRAM)
                  - returns driver buffer immediately
                              ↓
                  frame_pool  (decouples camera from network)
                              ↓
             HTTP stream handler  (Core 1) → Frigate MJPEG client
```

**Core assignments:**
- Core 0: Wi-Fi / lwIP stack
- Core 1: `cam_task`, `cam_psram_jpeg_task`, VSYNC ISR, GDMA EOF ISR, HTTP server tasks

---

## Hard Hardware Constraints

These are physical limits of the PY260 sensor and the ESP32-S3 OPI PSRAM bus.
Changing them will break the firmware.

### XCLK = 10 MHz — do not increase

The PY260's vertical front porch is approximately 3 μs at 10 MHz.

| XCLK | Front porch | Result |
|------|-------------|--------|
| 10 MHz | ~3 μs | Stable — 0 NO-SOI |
| 16 MHz | ~1.875 μs | Double-exception crash |
| 20 MHz | ~1.5 μs | 96% NO-SOI flood |

At higher XCLK the ISR cannot reach the GDMA before the sensor's sync window closes,
causing every captured buffer to start mid-frame with no JPEG SOI marker.

### PSRAM at 40 MHz — do not increase

At 80 MHz, OPI bus contention between camera GDMA writes and CPU PSRAM reads causes
DMA corruption. `CONFIG_SPIRAM_SPEED_40M=y` in `sdkconfig.defaults` is mandatory.

### `psram_mode = false`

The upstream M5Stack driver sets `cam_obj->psram_mode = (xclk_freq_hz == 16000000)`.
At 10 MHz this silently evaluates to `false`, which in the original driver means
frame buffers are allocated in internal DRAM (which cannot hold a 512 KB JPEG).
This firmware overrides that path: `fb_location = CAMERA_FB_IN_PSRAM` is set
directly and `psram_mode` is left `false` throughout. Testing with `psram_mode = true`
produced frame truncation with no throughput benefit.

### `SPIRAM_FETCH_INSTRUCTIONS` and `SPIRAM_RODATA` must stay disabled

Placing code or read-only data in PSRAM competes with camera DMA on the OPI bus.
These options are explicitly disabled in `sdkconfig.defaults`.

---

## ISR Requirements

The VSYNC ISR has two hard requirements that must not be relaxed.

### 1. Must run on Core 1

`ll_cam_init_isr()` is called via `esp_ipc_call_blocking(1, ...)` in `cam_hal.c`.

Why: Wi-Fi IRQs run on Core 0. If the VSYNC ISR is also assigned to Core 0, a
Wi-Fi interrupt can preempt it and delay it past the PY260's ~3 μs front porch.
Even a 5–10 μs delay produces a 100% NO-SOI flood. Pinning to Core 1 isolates
the ISR from all Wi-Fi activity.

### 2. Must execute the full reset sequence

`ll_cam_vsync_isr` in `ll_cam.c` performs this sequence on every frame:

```
cam_reset → cam_afifo_reset → in_rst → peri_sel → burst mode → cam_rec_data_bytelen
```

Why: Without the full reset, toggling `cam_start` generates a spurious second VSYNC
interrupt that cuts every captured frame to exactly 4096 bytes (2 DMA chunks).
A lighter ISR that only repoints the GDMA descriptor works correctly in
`FPS_TEST_MODE` (no Wi-Fi) but breaks immediately when Wi-Fi is active.

---

## Flash Write Safety Rule

**Never perform any flash write while the camera DMA is running.**

Any flash write — NVS commit, OTA partition write, `nvs_set_*` + `nvs_commit()` —
disables the OPI PSRAM cache for the duration of the write. The camera GDMA
continuously writes captured frame data to PSRAM frame buffers. If the cache-disable
window overlaps with a DMA write, the CPU gets ExcCause=7
(cache-disabled cached-memory access) → double exception → device crash.

This is why:
- OTA runs from `ota_mgr_run_pending()` **before** `esp_camera_init()` in `main.c`
- The OTA URL is passed across the reboot via `RTC_NOINIT_ATTR` (plain SRAM, no flash)
- NVS writes that happen at boot (recovery manager boot counter) complete before the
  camera initializes

---

## OTA Architecture: RTC NOINIT + Reboot

### Why not NVS for the OTA URL?

`nvs_commit()` writes flash → disables OPI PSRAM cache → camera DMA crashes.
See Flash Write Safety Rule above.

### Why `RTC_NOINIT_ATTR` and not `RTC_DATA_ATTR`?

`RTC_DATA_ATTR` places data in `.rtc.data`. The bootloader re-copies this section
from the flash image on every non-deep-sleep reset (`esp_image_format.c`:
`load_rtc_memory = reset_reason != DEEP_SLEEP`). After `esp_restart()`, any values
written to `.rtc.data` before the reboot are overwritten with zeros from the image
before the app runs. **`RTC_DATA_ATTR` is only for deep-sleep persistence.**

`RTC_NOINIT_ATTR` places data in `.rtc_noinit`, a `(NOLOAD)` section. The bootloader
has no image data to copy for it and skips it on every reset. Values survive
`esp_restart()` and crash resets — the three-field integrity check (two 32-bit
canaries + URL length) guards against stale state from panics.

### OTA flow

```
MQTT unitcams3/ota/set received
    → ota_mgr_start_url(): write URL to RTC_NOINIT_ATTR (pure SRAM, no flash)
    → recovery_mgr_signal_planned_reboot() (prevents boot-loop false positive)
    → esp_restart()

Next boot:
    → ota_mgr_run_pending() called BEFORE esp_camera_init()
    → Three-field integrity check passes → URL present
    → RTC RAM cleared (failure will not cause boot loop)
    → Validate magic byte 0xE9 on first chunk (reject 404 HTML, captive portals)
    → esp_ota_begin() → flash loop → content_length check → esp_ota_end()
    → esp_ota_set_boot_partition() → esp_restart()

Two minutes after clean boot:
    → recovery_mgr health timer fires
    → esp_ota_mark_app_valid_cancel_rollback()
```

---

## Recovery Manager: Boot-Loop Detection

The recovery manager increments an NVS `boot_count` on every boot and resets it
after 2 minutes of stable uptime (`esp_ota_mark_app_valid_cancel_rollback` is
also called at that point). If the device crashes and reboots 3 times before
the 2-minute timer fires, `boot_count` reaches the threshold and services are
suppressed (safe mode) to allow serial diagnostics.

**Planned reboot signalling:** OTA triggers and MQTT restart commands call
`recovery_mgr_signal_planned_reboot()` before `esp_restart()`. This writes a
magic value to a separate `RTC_NOINIT_ATTR` variable. On the next boot,
`recovery_mgr_init()` detects the magic, resets `boot_count` to zero, and clears
the flag — preventing deliberate reboots from tripping the boot-loop threshold.

---

## Component Map

| Component | Purpose |
|-----------|---------|
| `esp32-camera/` | Forked M5Stack driver — PY260/mega_ccm only, JPEG only, ISR on Core 1 |
| `frame_pool/` | Pre-allocates 4 × 512 KB PSRAM buffers at boot; ring-buffer semantics |
| `jpeg_validate/` | SOI/EOI boundary check + atomic drop counter |
| `http_server/` | Port 80: snapshot, health, stats, coredump; Port 81: MJPEG stream |
| `mqtt_mgr/` | MQTT client, HA auto-discovery, telemetry every 10 s, command handling |
| `ota_mgr/` | URL-based OTA via MQTT, RTC NOINIT URL storage, OTA callback registration |
| `recovery_mgr/` | NVS boot-loop detection, 2-min health timer, planned-reboot signalling |

---

## Driver Fork Divergences from Upstream

The `components/esp32-camera/` directory is a fork of
[espressif/esp32-camera](https://github.com/espressif/esp32-camera) with these
intentional changes:

1. **JPEG only** — all RAW/RGB565/YUV/Bayer capture paths removed. This
   significantly reduces ISR complexity and removes unreachable code paths for
   this sensor.

2. **Core 1 ISR** — `ll_cam_init_isr()` called via `esp_ipc_call_blocking(1, ...)`
   instead of being registered on the calling core. Required for Wi-Fi coexistence.

3. **Full VSYNC ISR reset sequence** — the lighter "repoint GDMA only" ISR
   variant was tested and produces 4096-byte truncated frames when Wi-Fi is active.

4. **`psram_mode` trigger fixed** — upstream triggers PSRAM mode at
   `xclk_freq_hz == 16000000`; at 10 MHz this silently allocates frame buffers
   in internal DRAM. This fork uses `fb_location` directly.

5. **GDMA burst mode** — `in_ext_mem_bk_size = 2` (64-byte PSRAM bursts),
   `indscr_burst_en = 1`, `in_data_burst_en = 1` re-applied in ISR after every
   `in_rst`. Reduces OPI bus transactions 4×.

6. **Extended EOI search window** — the GDMA does not update `descriptor.length`
   for the final partial DMA chunk, but pixel data is written. EOI (`FF D9`) lives
   in this chunk; the search window is extended by one `dma_half_buffer_size`.

7. **`cam_take()` iterative** — converted from recursive to iterative to prevent
   stack overflow under high frame-drop conditions.

### Deep Driver Analysis Archive

For the full forensic write-up (schematic cross-checks, upstream diffs, and
priority-ranked fix backlog), see:
`docs/internal/driver_production_improvements.md`

This file is archival/reference material for driver contributors and upstream PR
prep. It is not required reading for normal firmware setup/operation.

---

## Partition Layout

```
nvs         0x9000    32 KB
otadata     0x11000    8 KB
phy_init    0x13000    4 KB
ota_0       0x20000    5 MB  ← active at first boot
ota_1       0x520000   5 MB  ← OTA target
coredump    0xA20000 128 KB  ← ELF format, GET /api/coredump
```

No factory partition. Both OTA slots are 5 MB to accommodate the BLE stack
(NimBLE adds ~400 KB to the binary).

---

## Key Files for Common Tasks

| Task | File |
|------|------|
| Camera resolution / quality | `GET /setup` browser page (persisted to NVS); compile-time defaults in `main/main.c` |
| MQTT URL / credentials / device ID | `GET /setup` browser page (persisted to NVS); compile-time defaults via `idf.py menuconfig` |
| Add HTTP endpoint | `components/http_server/http_server.c` |
| Add MQTT sensor to HA | `components/mqtt_mgr/mqtt_mgr.c` — `send_ha_discovery()` |
| Tune recovery thresholds | `components/recovery_mgr/recovery_mgr.c` — `recovery_config_t` |
| Debug ISR / DMA issues | `components/esp32-camera/target/esp32s3/ll_cam.c` |
| Debug frame capture | `components/esp32-camera/driver/cam_hal.c` |
