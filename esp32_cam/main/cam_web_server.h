#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the Web Server on port 80:
 * - /         : Full HTML5 Web UI for Camera live view, controls & OTA
 * - /stream   : Live MJPEG video stream (viewable in browser or VLC)
 * - /capture  : Single JPEG snapshot
 * - /api/...  : Status, Logs, Flash, Framesize, OTA
 */
esp_err_t cam_web_server_start(const char *device_id);

#ifdef __cplusplus
}
#endif
