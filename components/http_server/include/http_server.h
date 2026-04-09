#pragma once

#include "esp_err.h"
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

// Stops both servers
void http_server_stop(void);

// Signals both servers to stop streaming/handling (for OTA)
void http_server_signal_stop(void);

// Prepares the HTTP environment for OTA (drains queues, de-inits camera)
void http_server_prepare_ota(void);

// Returns the number of active MJPEG stream clients
uint8_t http_server_get_active_streams(void);

// Returns number of camera_reinit() calls triggered by the broadcaster
uint32_t http_server_get_reinit_count(void);

// Set broadcaster FPS cap (0 = unlimited, 1-15 = max fps delivered to clients).
// Takes effect immediately. Not persisted across reboot.
void http_server_set_fps_cap(uint8_t fps_cap);

#ifdef __cplusplus
}
#endif
