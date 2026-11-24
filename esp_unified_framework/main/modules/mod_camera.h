#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mod_camera_init(void);
esp_err_t mod_camera_register_routes(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
