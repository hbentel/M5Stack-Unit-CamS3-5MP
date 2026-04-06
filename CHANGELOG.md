# Changelog

All notable changes to this project will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [v0.2.1] — 2026-04-05

### Added

- **Multi-client MJPEG streaming** — support for up to 5 simultaneous stream clients
  (e.g., Frigate `detect` + `record` + browser view) using asynchronous HTTP
  handlers and a dedicated broadcaster task.
- **`active_streams` metric** — added to `/stats` JSON and MQTT telemetry;
  includes Home Assistant auto-discovery as a new sensor.
- **Broadcaster idling** — `mjpeg_broadcaster_task` now idles when 0 clients are
  connected, preserving PSRAM bandwidth and reducing heat.
- **Python test script** — `test_mjpeg_concurrency.py` added for verifying
  multi-client performance.

### Fixed

- **Internal RAM for stacks** — forced all task stacks to internal SRAM to prevent
  Double Exception crashes during flash writes (NVS/OTA).
- **Startup stability** — increased main task stack to 16KB and system event stack
  to 4096B to prevent overflows during mDNS and MQTT initialization.
- **mDNS unique instances** — assigned unique instance names ("HTTP", "Stream")
  to prevent "Service already exists" errors on port 81.
- **Race condition fix** — delayed telemetry task until MQTT is connected to
  ensure all mutexes are initialized before use.
- **Thread-safe camera recovery** — added a mutex to `camera_reinit()` to prevent
  concurrent driver de-initialization crashes.
- **Frame pool capacity** — increased from 4 to 8 buffers (4MB total PSRAM) to
  smoothly handle multiple simultaneous consumers.

## [v0.2.0] — 2026-04-04

### Added

- **mDNS** — device advertises at `<device-id>.local` using the espressif/mdns
  managed component. Hostname tracks the Device ID set on `/setup`.
- **Web log viewer** (`GET /api/logs`) — 16 KB PSRAM ring buffer captures all
  `ESP_LOG*` output from boot onward; returned as plain text with ANSI colour codes.
  Implemented via `esp_log_set_vprintf()` hook with ISR-safe spinlock. Gracefully
  disabled if PSRAM is unavailable at boot.
- **`reset_reason` in `/health`** — JSON field showing why the device last rebooted
  (`power_on`, `software`, `panic`, `task_watchdog`, etc.).
- **Device ID tooltip on `/setup`** — explains that Device ID controls the MQTT
  topic prefix, Home Assistant entity prefix, and mDNS hostname (`<id>.local`).

### Fixed

- **Resolution enum mismatch** — `/setup` dropdown values now map to PY260-native
  framesizes only: QVGA (6), VGA (10), HD (13), UXGA (15). Previously the list
  included CIF and HVGA which silently fail on this sensor.
- **Invalid NVS cam_res recovery** — `config_mgr_init()` validates the stored
  resolution on boot and resets to VGA if the value is not in the supported set,
  preventing boot loops from stale NVS written by older firmware.
- **`mega_ccm` unsupported framesize** — added an `else` branch that logs
  `ESP_LOGE` and returns `-1` instead of silently succeeding.
- **MQTT HA discovery ranges** — corrected brightness to `0–8`, contrast and
  saturation to `0–6` to match the PY260 driver limits.
- **`assert()` in `ll_cam.c`** replaced with `ESP_LOGE` + `ESP_ERR_INVALID_ARG`
  return (assertions are compiled out in release builds).
- **Deinit ordering in `cam_hal.c`** — `ll_cam_deinit()` (frees ISR) now called
  before `vQueueDelete()`.
- **`CONFIG_CAMERA_TASK_STACK_SIZE` Kconfig** — default raised to 8192; now
  wired to `CAM_TASK_STACK` in `cam_hal.c`.
- **GDMA DMA buffer size range** extended to 65536 in Kconfig.
- **`DEFAULT_CAM_RES`** corrected from 8 (CIF) to 10 (VGA) in `config_mgr.c`.
- **`RECOVERY_ERR_RTSP_SEND` renamed** to `RECOVERY_ERR_STREAM_SEND`.
- **Orphaned `CONFIG_APP_UPDATE=y`** removed from `sdkconfig.defaults`.

### Build / CI

- GitHub Actions now builds both IDF v5.3.2 and IDF v6.0 on every push.
- Release artifacts include `unitcams3_merged.bin` (single-file flash) with
  build provenance attestation via `actions/attest-build-provenance`.

---

## [v0.1.0] — 2026-03-03

Initial public release.

### Features

- **Camera driver** — Forked ESP32-S3 camera driver tuned for the PY260 (`mega_ccm`)
  sensor on the M5Stack Unit CamS3-5MP. JPEG-only capture; all RAW/RGB/YUV paths removed.
- **ISR stability** — VSYNC ISR pinned to Core 1 via `esp_ipc_call_blocking`; full
  GDMA reset sequence prevents spurious double-VSYNC that truncated frames to 4096 bytes.
  XCLK hard-limited to 10 MHz (PY260 front-porch constraint).
- **Frame pool** — 4 × 512 KB PSRAM ring buffer decouples camera capture from HTTP delivery.
- **JPEG validation** — SOI/EOI byte-level check with atomic drop counters exposed via `/stats`.
- **HTTP server** (port 80) — `GET /` snapshot, `GET /health` (JSON, includes ELF SHA-256),
  `GET /stats`, `GET /api/coredump` (ELF coredump download).
- **MJPEG stream** (port 81) — `GET /stream` multipart MJPEG, compatible with Frigate NVR
  (`input_args: -f mjpeg`).
- **MQTT + Home Assistant discovery** — Telemetry every 10 s (RSSI, heap, FPS, drop counters);
  image controls (brightness, contrast, saturation, WB mode); reboot command.
- **BLE Wi-Fi provisioning** — First-boot BLE provisioning via `wifi_provisioning`; credentials
  stored in NVS. No hardcoded Wi-Fi credentials.
- **Runtime config (`/setup` page)** — Browser form to change MQTT URL/credentials, device ID,
  MQTT enable toggle, camera resolution, JPEG quality. Values persist in NVS; device restarts
  to apply.
- **OTA updates** — URL-based OTA triggered by MQTT (`unitcams3/ota/set`). Downloads full
  firmware to PSRAM, stops Wi-Fi, then flashes — avoids OPI cache-disable crash during
  concurrent camera DMA.
- **Recovery manager** — NVS boot-loop detection (threshold = 3), safe mode, 2-minute health
  timer, OTA rollback confirmation via `esp_ota_mark_app_valid_cancel_rollback`.
- **IDF v5 and v6 support** — `build.sh` targets IDF v5.3.2 LTS; `build_v6.sh` targets
  IDF v6 beta. Both auto-detect version mismatch via `CMakeCache.txt` and clean before rebuild.
- **CI** — GitHub Actions workflow builds firmware with IDF v5.3.2 on every push and PR.
