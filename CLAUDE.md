# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash Commands

```bash
bash build.sh              # Build only
bash build.sh clean        # Full clean then build
bash flash.sh              # Flash only (use this to push firmware)
bash monitor.sh            # Serial monitor only — Ctrl+] to exit
bash flashmon.sh           # Build + flash + monitor (interactive terminal only)
```

**CRITICAL — never pipe flash scripts through `head`, `tail`, or any pipe.** esptool erases all flash regions before writing any of them. If the process is killed mid-write, the partition table is erased but not rewritten and the device won't boot. If this happens, run `bash flash.sh` immediately to recover.

Recommended workflow: `bash flash.sh` in one terminal, `bash monitor.sh` in a second terminal.

The device auto-detected port is `/dev/cu.usbmodem*` (native USB CDC). All scripts handle this automatically.

## Testing After Flash

```bash
curl http://192.168.50.44/health   # Uptime, heap, PSRAM, jpeg_drops
curl http://192.168.50.44/stats    # Camera FPS, drop counters, Wi-Fi RSSI, memory
curl -o /tmp/snap.jpg http://192.168.50.44/   # Single JPEG snapshot
```

The `/stats` FPS field is a delta — call it twice 5–10s apart to get a real reading.

## Architecture Overview

### Dual-Core Data Flow

```
PY260 sensor → GDMA → PSRAM frame buffers (4×~512KB)
                           ↓
                    cam_psram_jpeg_task (Core 1)
                    - validates SOI/EOI
                           ↓
                    mjpeg_broadcaster_task (Core 1)
                    - idles when 0 clients connected
                    - copies to frame_pool (8×512KB PSRAM)
                    - signals active worker tasks
                           ↓
                    mjpeg_client_worker_task (Core 1)
                    - 1 task per connection (max 5)
                    - sends from frame_pool to socket
                           ↓
                    Frigate / Browser MJPEG client
```

**Core assignments:**
- Core 0: Wi-Fi / lwIP stack
- Core 1: cam_task, cam_psram_jpeg_task, mjpeg_broadcaster_task, mjpeg_worker_task, VSYNC ISR, GDMA EOF ISR, HTTP server tasks

### ISR Requirements (do not change)

The VSYNC ISR has two hard requirements that must not be removed:

1. **Must run on Core 1.** `ll_cam_init_isr()` is called via `esp_ipc_call_blocking(1, ...)` in `cam_hal.c`. Without this, Wi-Fi IRQs on Core 0 delay the ISR past the PY260's ~3μs vertical front porch, causing 100% NO-SOI floods.

2. **Must execute the full reset sequence.** `ll_cam_vsync_isr` in `ll_cam.c` performs `cam_reset + cam_afifo_reset + in_rst + peri_sel + burst mode + cam_rec_data_bytelen` on every frame. Without this sequence, `cam_start` toggling generates a spurious second VSYNC interrupt that cuts every frame to 4096 bytes (2 DMA chunks). The lighter version (repoint GDMA only) was tested and breaks immediately when Wi-Fi is active.

### Hard Hardware Constraints

- **XCLK = 10MHz — do not increase.** The PY260's vertical front porch is ~3μs at 10MHz. At 16MHz it's ~1.875μs (causes double exception crash), at 20MHz ~1.5μs (96% NO-SOI flood). 10MHz is a physical sensor ceiling.
- **PSRAM at 40MHz** — not 80MHz. Higher speed causes OPI bus contention with camera DMA.
- **Task Stacks in Internal RAM** — `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=n` is mandatory. Stacks in PSRAM cause Double Exception crashes during flash writes (NVS/OTA).
- **Main Task Stack = 16KB** — required for heavy initialization.
- **Task WDT Panic = disabled** — `CONFIG_ESP_TASK_WDT_PANIC=n` to allow diagnostic recovery during startup.
- **`psram_mode = false`** — `psram_mode=true` was tested and causes frame truncation bugs. Do not re-enable.
- **`SPIRAM_FETCH_INSTRUCTIONS` and `SPIRAM_RODATA` must stay disabled** — code/rodata in PSRAM competes with camera DMA on the OPI bus.
- **`FPS_TEST_MODE` in `main.c` line 24** — must be `0` for production. Set to `1` only when measuring driver performance without Frigate connected.

### Component Responsibilities

| Component | Purpose |
|-----------|---------|
| `esp32-camera/` | Forked M5Stack driver (PY260/mega_ccm only, JPEG only, ISR on Core 1) |
| `frame_pool/` | Pre-allocates 8×512KB PSRAM buffers at boot; decouples camera from HTTP |
| `jpeg_validate/` | Application-level SOI/EOI check + atomic drop counter |
| `log_buf/` | 16KB PSRAM ring buffer via `esp_log_set_vprintf()` hook; `log_buf_snapshot()` serves `/api/logs` |
| `http_server/` | Port 80: snapshot `/`, health `/health` (incl. `reset_reason`), stats `/stats`, coredump `/api/coredump`, logs `/api/logs`; Port 81: MJPEG stream `/stream` |
| `ota_mgr/` | URL-based OTA via MQTT — publish firmware URL to `unitcams3/ota/set` |
| `mqtt_mgr/` | MQTT client, HA auto-discovery, telemetry every 10s, command handling |
| `recovery_mgr/` | NVS boot-loop detection (threshold=3), safe mode, 2-min health timer, OTA rollback confirmation |

### HTTP Ports Summary

| Port | Server | Endpoints |
|------|--------|-----------|
| 80 | Main | `GET /` snapshot, `GET /health` (JSON incl. `app_sha256`, `reset_reason`), `GET /stats`, `GET /api/coredump`, `GET /api/logs` |
| 81 | Stream | `GET /stream` MJPEG (used by Frigate) |

### MQTT Topics

Base topic: `unitcams3`

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `unitcams3/status` | publish | `ON`/`OFF` (LWT) |
| `unitcams3/rssi`, `/uptime`, `/heap`, `/psram_free` | publish | Telemetry every 10s |
| `unitcams3/fps`, `/no_soi`, `/jpeg_drops`, `/recovery_count`, `/streams` | publish | Camera health/stats every 10s |
| `unitcams3/ota_status` | publish | `idle` / `pending_reboot` |
| `unitcams3/brightness/set`, `/contrast/set`, `/saturation/set`, `/wb_mode/set` | subscribe | Camera image controls |
| `unitcams3/restart` | subscribe | Triggers `esp_restart()` |
| `unitcams3/ota/set` | subscribe | URL string → triggers URL-based OTA |

### OTA Flow (RTC RAM + Reboot Architecture)

**Why not NVS?** `nvs_commit()` writes flash and disables the OPI PSRAM cache — the same mechanism that causes OTA flash crashes. If camera DMA is active when any flash write disables the cache, the CPU gets ExcCause=7 (cache-disabled cached-memory access) → double exception. This was the root cause of all OTA crashes observed during development.

**Why RTC RAM?** `RTC_NOINIT_ATTR` places data in `.rtc_noinit` — a `(NOLOAD)` section with no flash image. The bootloader skips it on every reset, so values survive `esp_restart()`. **Do not use `RTC_DATA_ATTR` for this** — the bootloader re-copies `.rtc.data` from flash on every non-deep-sleep reset, overwriting values written before the reboot. `RTC_DATA_ATTR` is for deep-sleep persistence only.

**OTA flow:**
1. MQTT `unitcams3/ota/set` received → `ota_mgr_start_url()` writes URL to RTC RAM (pure SRAM, no flash) → `esp_restart()`
2. Next boot: `ota_mgr_run_pending()` called from `main.c` after Wi-Fi, **before** `esp_camera_init()`
3. If URL found in RTC RAM (integrity check passes): validates magic byte (0xE9), downloads firmware, validates content_length, flashes, reboots
4. If OTA fails: RTC RAM cleared (no boot loop), normal boot continues — retrigger via MQTT
5. On success: reboots into new firmware; `recovery_mgr` marks healthy after 2 min via `esp_ota_mark_app_valid_cancel_rollback()`

**Protections:**
- Magic byte 0xE9 checked on first chunk before erasing target partition (rejects HTML 404 pages, captive portals)
- `image_len == content_length` verified before `esp_ota_end()` (rejects truncated downloads)
- RTC RAM cleared before attempting OTA (crashed mid-flash does not cause boot loop)
- Two 32-bit canary values + URL length field guard against stale RTC state from previous panics
- 4KB download buffer is `heap_caps_malloc(MALLOC_CAP_INTERNAL)` — PSRAM cache disabled during flash writes

**Verify OTA flashed the correct firmware:**
```bash
BUILD_SHA=$(shasum -a 256 build/unitcams3_firmware.elf | cut -c1-64)
DEVICE_SHA=$(curl -s http://192.168.50.44/health | python3 -c "import sys,json; print(json.load(sys.stdin)['app_sha256'])")
[ "$BUILD_SHA" = "$DEVICE_SHA" ] && echo "MATCH" || echo "MISMATCH"
```
`/health` includes `app_sha256` — the SHA-256 of the ELF embedded in the running firmware at build time.

### Partition Layout

```
nvs         0x9000   32KB
otadata     0x11000   8KB
phy_init    0x13000   4KB
ota_0       0x20000   5MB   ← active partition
ota_1       0x520000  5MB   ← OTA target
coredump    0xA20000 128KB  ← ELF format, download via GET /api/coredump
```

### Circular Dependency Resolution

`mqtt_mgr` and `ota_mgr` would create a circular link dependency if they directly required each other. Resolved with a callback: `ota_mgr_init()` calls `mqtt_mgr_register_ota_callback(ota_mgr_start_url)`. `mqtt_mgr` calls through the callback without a compile-time dependency on `ota_mgr`.

## Key Files for Common Tasks

| Task | Files |
|------|-------|
| Change Wi-Fi credentials | Flash device fresh (clears NVS) or erase `wifi_prov` NVS key; device re-enters BLE provisioning on next boot |
| Change camera resolution/quality | `main/main.c` — `CAM_FRAME_SIZE`, `jpeg_quality` in `camera_config` |
| Change image settings (brightness/WB/etc.) | `main/main.c` — `CAM_*` defines + `apply_camera_settings()` |
| Add HTTP endpoint | `components/http_server/http_server.c` — add handler + register in `start_http_server()` |
| Add MQTT sensor to HA | `components/mqtt_mgr/mqtt_mgr.c` — `send_ha_discovery()` + `mqtt_telemetry_task()` |
| Tune camera recovery | `components/recovery_mgr/recovery_mgr.c` — `recovery_config_t` defaults |
| Debug ISR/DMA issues | `components/esp32-camera/target/esp32s3/ll_cam.c` — `ll_cam_vsync_isr()` |
| Debug frame capture | `components/esp32-camera/driver/cam_hal.c` — `cam_psram_jpeg_task()`, `cam_verify_jpeg_soi/eoi()` |
