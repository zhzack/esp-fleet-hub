#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);
httpd_handle_t web_server_get_handle(void);

#ifdef __cplusplus
}
#endif
