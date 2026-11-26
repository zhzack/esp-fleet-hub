#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_DISPLAY_MAX_LINES APP_DISPLAY_MAX_LINES

esp_err_t hal_display_init(void);
esp_err_t hal_display_set_line(int row, const char *text);
esp_err_t hal_display_clear(void);
esp_err_t hal_display_set_backlight(int percent);
void hal_display_apply_config(void);
void hal_display_apply_config_ex(bool force_full, bool clear_only);
void hal_display_populate_status_cjson(void *cjson_obj);
const char *hal_display_get_line(int row);
int hal_display_get_backlight(void);
bool hal_display_is_ready(void);
const char *hal_display_driver_name(void);

#ifdef __cplusplus
}
#endif
