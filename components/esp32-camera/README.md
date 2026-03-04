# esp32-camera (UnitCamS3 PY260 Fork)

This component is a project-specific fork of Espressif/M5Stack `esp32-camera`
for the **M5Stack Unit CamS3-5MP** (ESP32-S3 + PY260/mega_ccm sensor).

It is intentionally narrowed to one board and one sensor path to prioritize
reliability under Wi-Fi streaming load.

## Scope

- Target SoC: **ESP32-S3**
- Target sensor: **PY260 (mega_ccm)**
- Capture mode: **JPEG only**
- Primary stream integration: HTTP MJPEG in parent project

Not a generic multi-sensor camera component.

## Critical Constraints (Do Not Break)

1. **XCLK is fixed at 10 MHz for PY260 stability**.
2. **Camera ISR allocation must be pinned to Core 1** to avoid Wi-Fi IRQ latency.
3. **Full VSYNC reset sequence is required** in `ll_cam_vsync_isr`.
4. **PSRAM must run at 40 MHz** at project level.
5. **Flash writes must not race camera DMA** (handled in app architecture).

See root architecture notes for full rationale:
`../../ARCHITECTURE.md`

## Key Fork Changes vs Upstream

- JPEG-only path retained; non-required sensor paths removed.
- VSYNC/GDMA ISR behavior hardened for Wi-Fi coexistence.
- Core-1 ISR allocation via IPC.
- `cam_take()` made iterative to avoid recursive stack growth.
- PSRAM JPEG path includes cache coherency handling and extended EOI search.
- Frame buffer placement driven by `fb_location` (not XCLK side effects).
- 64-byte DMA alignment/burst behavior tuned for ESP32-S3 + OPI PSRAM.

## IDF Compatibility

SCCB backend is selected by IDF major version in `CMakeLists.txt`:

- IDF 5.x: `driver/sccb.c` (legacy I2C API)
- IDF 6.x+: `driver/sccb_i2c_master.c` (new I2C master API)

## Component Layout

- `driver/esp_camera.c`: probe/init/deinit, fb API, NVS save/load
- `driver/cam_hal.c`: capture pipeline, task/queue flow, stats
- `target/esp32s3/ll_cam.c`: LCD_CAM/GDMA low-level configuration and ISR
- `sensors/mega_ccm.c`: PY260 sensor control implementation
- `driver/sccb*.c`: SCCB transport backends

## Public APIs Used by Parent Project

- `esp_camera_init()` / `esp_camera_deinit()`
- `esp_camera_fb_get()` / `esp_camera_fb_return()`
- `esp_camera_sensor_get()`
- `esp_camera_get_stats()`

## Contributor Notes

- This component is optimized for production behavior on UnitCamS3-5MP,
  not broad compatibility.
- If you change ISR timing, clocking, DMA sizing, or cache behavior,
  revalidate with Wi-Fi active and MJPEG streaming.
- Keep documentation in sync with architecture constraints after any low-level
  changes.

## Upstream Base

- Espressif `esp32-camera`: https://github.com/espressif/esp32-camera
- M5Stack UnitCamS3 reference lineage: `UnitCamS3-UserDemo` (`unitcams3-5mp`)
