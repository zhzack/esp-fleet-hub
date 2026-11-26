#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);
httpd_handle_t web_server_get_handle(void);

/**
 * @brief Verify optional API Token (X-API-Key / Bearer token / ?token=)
 *        Sends HTTP 401 and returns false if unauthorized.
 */
bool web_server_check_auth(httpd_req_t *req);
bool web_server_check_read_auth(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
