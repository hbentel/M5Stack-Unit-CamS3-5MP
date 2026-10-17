#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the main server on Port 80 (Snapshots, Health, Coredump)
esp_err_t start_http_server(void);

// Starts the stream server on Port 81 (MJPEG Stream)
esp_err_t start_stream_server(void);

// Returns handle to Port 80 server
httpd_handle_t http_server_get_handle(void);

// Returns handle to Port 81 server
httpd_handle_t stream_server_get_handle(void);

// Returns the number of active MJPEG stream clients
uint8_t http_server_get_active_streams(void);

// Returns number of camera_reinit() calls triggered by the broadcaster
uint32_t http_server_get_reinit_count(void);

// Set broadcaster FPS cap (0 = unlimited, 1-15 = max fps delivered to clients).
// Takes effect immediately. Not persisted across reboot.
void http_server_set_fps_cap(uint8_t fps_cap);

// Pause/resume camera use by the broadcaster and snapshot handler, so the camera
// can be deinitialized safely. Poll http_server_camera_idle() after pausing.
void http_server_pause_camera(bool pause);
bool http_server_camera_idle(void);

#ifdef __cplusplus
}
#endif
