#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_mgr_init(void);

/**
 * @brief HTTP POST handler for streaming OTA upload
 */
esp_err_t ota_upload_post_handler(httpd_req_t *req);

/**
 * @brief Get partition status JSON
 */
size_t ota_mgr_status_to_json(char *dst, size_t max_len);

#ifdef __cplusplus
}
#endif
