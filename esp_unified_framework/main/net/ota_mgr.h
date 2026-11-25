#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize OTA manager and start 90s auto-rollback verification if PENDING_VERIFY
 */
esp_err_t ota_mgr_init(void);

/**
 * @brief HTTP POST handler for streaming OTA upload (/api/ota/upload, supports X-Firmware-SHA256)
 */
esp_err_t ota_upload_post_handler(httpd_req_t *req);

/**
 * @brief HTTP POST handler to trigger background Pull OTA (/api/ota/pull)
 */
esp_err_t ota_pull_post_handler(httpd_req_t *req);

typedef void (*ota_progress_cb_t)(const char *status, int progress_pct, int bytes_read, int total_bytes, const char *err);

/**
 * @brief Register optional OTA progress callback (e.g. for WebSocket ota_progress frames)
 */
void ota_mgr_set_progress_cb(ota_progress_cb_t cb);

/**
 * @brief Pull firmware binary over HTTP(S), verify 0xE9 & optional SHA-256, and switch boot partition
 */
esp_err_t ota_mgr_pull_from_url(const char *url, const char *expected_sha256, bool reboot_after);

/**
 * @brief Start asynchronous background Pull OTA job (reused by HTTP /api/ota/pull & WS ota.start)
 */
esp_err_t ota_mgr_start_pull_async(const char *url, const char *expected_sha256);

/**
 * @brief Get partition & rollback status JSON
 */
size_t ota_mgr_status_to_json(char *dst, size_t max_len);

#ifdef __cplusplus
}
#endif
