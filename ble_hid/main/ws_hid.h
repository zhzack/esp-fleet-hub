#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handle an incoming WebSocket frame on /ws/hid.
 * Parses binary or JSON HID packets and forwards them to USB or BLE.
 */
esp_err_t ws_hid_handle_frame(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
