#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mod_display_init(void);
esp_err_t mod_display_register_routes(httpd_handle_t server);

cJSON *mod_display_build_status_json(void);
esp_err_t mod_display_update_json(const char *json_str, bool switch_text_on_lines);

#ifdef __cplusplus
}
#endif
