# Changelog

All notable changes to this project will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
