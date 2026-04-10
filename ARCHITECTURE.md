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
                  - pushes latest pointer to Broadcaster
                              ↓
                  mjpeg_broadcaster_task (Core 1)
                  - idles when 0 clients connected
                  - copies camera frame to frame_pool (one copy, unavoidable)
                  - sets s_broadcast_fb = new buffer (ref_count = 1)
                  - signals active worker tasks via binary semaphores
                  - registered with Task WDT (30 s panic on hang)
                              ↓
                  mjpeg_client_worker_task (Core 1)
                  - grabs atomic ref to s_broadcast_fb (no copy)
                  - all workers share the same PSRAM buffer via ref count
                  - 1 task per connection (max 5)
                  - sends from frame_pool buffer directly to socket
                  - SO_SNDTIMEO (10 s) handles stalled clients gracefully
                  - frame_pool_unref() on completion → buffer returns to pool
                    when ref_count reaches 0
                              ↓
                  Frigate / Browser MJPEG client
```

**Frame pool sizing:** 3 slots × 512 KB = 1.5 MB PSRAM. Peak simultaneous usage
is 2 slots (broadcaster's current frame + workers mid-send). The 3rd slot covers
the transitional moment when the broadcaster has written a new frame but not yet
unreffed the old one. All workers share a single ref to the same buffer — no
per-worker copy is made.

**Core assignments:**
- Core 0: Wi-Fi / lwIP stack
- Core 1: `cam_task`, `cam_psram_jpeg_task`, `mjpeg_broadcaster_task`, `mjpeg_worker_task`, VSYNC ISR, GDMA EOF ISR, HTTP server tasks

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

## Memory Segregation (Stack Safety)

This firmware uses a strict memory allocation strategy to ensure stability on the ESP32-S3:

- **Task Stacks (Internal SRAM ONLY)**: 
  All FreeRTOS task stacks must be allocated in internal SRAM (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=n`).
  *Reason*: Flash writes (OTA, NVS commit) suspend the OPI PSRAM cache. If a task stack is in PSRAM during a flash write, the CPU triggers an immediate `Double Exception` crash when it tries to access the stack.

- **Main Task Stack (16 KB)**: 
  The main initialization task stack is increased to 16 KB to accommodate heavy simultaneous startup of mDNS, MQTT, and the Camera driver.

- **Frame Buffers (PSRAM ONLY)**: 
  Large image data is kept in PSRAM to prevent internal memory exhaustion. The `frame_pool` uses 3 × 512 KB buffers (1.5 MB total). With atomic reference counting, workers share refs to the same buffer rather than holding private copies, so 3 slots are sufficient for peak load.

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
MQTT unitcams3/ota/set received (JSON payload)
    → Token verified against config_mgr_get_ota_token() (if configured)
    → SHA-256 hex and URL extracted from JSON
    → ota_mgr_start_url(url, sha256_hex): write URL + hash to RTC_NOINIT_ATTR (pure SRAM, no flash)
    → recovery_mgr_signal_planned_reboot() (prevents boot-loop false positive)
    → esp_restart()

Next boot:
    → ota_mgr_run_pending() called BEFORE esp_camera_init()
    → Three-field integrity check passes → URL and hash copied to local stack buffers
    → RTC RAM cleared (failure will not cause boot loop)
    → Download full firmware to PSRAM buffer (fw_buf, up to 4 MB)
    → Validate magic byte 0xE9 on first chunk (reject 404 HTML, captive portals)
    → SHA-256 of fw_buf verified against saved hash (if provided) — abort if mismatch
    → content_length == downloaded size check
    → Wi-Fi stopped (no ISRs during flash)
    → esp_ota_begin() → flash loop (4 KB internal SRAM staging buffer) → esp_ota_end()
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
| `frame_pool/` | 3 × 512 KB PSRAM buffers; C11 atomic ref counting (`frame_pool_ref`/`frame_pool_unref`) for zero-copy multi-client delivery |
| `jpeg_validate/` | SOI/EOI boundary check + atomic drop counter |
| `http_server/` | Port 80: snapshot, health (incl. `reset_reason`), stats, coredump, logs; Port 81: MJPEG stream |
| `log_buf/` | 16 KB PSRAM ring buffer hooked into `esp_log_set_vprintf()`; exposes `log_buf_snapshot()` for `/api/logs` |
| `mqtt_mgr/` | MQTT client, HA auto-discovery, telemetry every 10 s, command handling |
| `ota_mgr/` | URL-based OTA via MQTT, RTC NOINIT URL storage, OTA callback registration |
| `recovery_mgr/` | NVS boot-loop detection, 2-min health timer, planned-reboot signalling |

---

## Wi-Fi Reliability

### TX Power

Do **not** cap Wi-Fi TX power with `esp_wifi_set_max_tx_power()`. The default
ceiling (~20 dBm) is required for reliable uplink. Capping at 32 units (8 dBm)
reduces transmit power by 12 dBm — a 16× reduction — causing the AP to
aggressively drop the client even when the downlink RSSI appears healthy. The
asymmetry between downlink (AP → device) and uplink (device → AP) RSSI means a
−56 dBm downlink can coexist with an unacceptable uplink at 8 dBm TX.

### Reconnect Strategy

`WIFI_PS_NONE` (power save disabled) is mandatory. Power save mode allows the AP
to buffer packets between beacon intervals, introducing latency spikes that stall
the MJPEG stream and trigger `SO_SNDTIMEO` timeouts on worker tasks.

A 1 s backoff delay before `esp_wifi_connect()` retry prevents rapid reconnect
storms. Without it, a briefly-unavailable AP receives a flood of auth requests and
may rate-limit or ban the device.

### IP Loss

Both `IP_EVENT_STA_GOT_IP` and `IP_EVENT_STA_LOST_IP` must be handled.
`STA_LOST_IP` fires when the DHCP lease is not renewed but no `STA_DISCONNECTED`
event is generated — without this handler the device remains Wi-Fi associated but
has no IP, silently stalling all HTTP and MQTT traffic.

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
| Change mDNS hostname | Change Device ID on `/setup` — hostname is set from `config_mgr_get_device_id()` at boot |
| Add HTTP endpoint | `components/http_server/http_server.c` |
| Add MQTT sensor to HA | `components/mqtt_mgr/mqtt_mgr.c` — `send_ha_discovery()` |
| Tune recovery thresholds | `components/recovery_mgr/recovery_mgr.c` — `recovery_config_t` |
| Debug ISR / DMA issues | `components/esp32-camera/target/esp32s3/ll_cam.c` |
| Debug frame capture | `components/esp32-camera/driver/cam_hal.c` |
| View runtime logs without serial | `curl http://<device-ip>/api/logs` — `components/log_buf/log_buf.c` |
