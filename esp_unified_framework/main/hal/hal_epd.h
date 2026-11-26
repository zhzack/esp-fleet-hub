#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_EPD_PANEL_213_BW = 0,   // 2.13" 122x250 (RAM 128x250) Black/White SSD1680/SSD1683 (GDEY0213B74)
    HAL_EPD_PANEL_213_BWR = 1,  // 2.13" 122x250 (RAM 128x250) Black/White/Red SSD1680/SSD1683 (GDEY0213Z98 / FPC-A002)
    HAL_EPD_PANEL_420_BWR = 2,  // 4.2"  400x300 Black/White/Red SSD1683 (GDEY042Z98)
} hal_epd_panel_t;

typedef struct {
    bool initialized;
    bool refreshing;
    bool busy_timeout;
    bool has_red;
    bool supports_partial;
    bool has_image;
    bool image_has_red;
    hal_epd_panel_t panel;
    const char *panel_id;
    const char *driver_name;
    uint16_t native_w;
    uint16_t native_h;
    uint16_t width;
    uint16_t height;
    uint16_t image_w;
    uint16_t image_h;
    uint32_t refresh_count;
    uint32_t last_refresh_ms;
    int pin_mosi;
    int pin_clk;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
} hal_epd_info_t;

bool hal_epd_is_epd_panel_name(const char *panel_name);
hal_epd_panel_t hal_epd_parse_panel(const char *panel_name);
const char *hal_epd_panel_name(hal_epd_panel_t panel);

esp_err_t hal_epd_init(void);
esp_err_t hal_epd_reconfigure(void);
void hal_epd_deinit(void);

esp_err_t hal_epd_request_refresh(bool force_full, bool clear_only);
esp_err_t hal_epd_apply_image_json(const char *json_str, bool *out_force_full);
void hal_epd_update_metrics(float cpu, float ram, float disk, const char *dl, const char *ul, const char *uptime, const char *time_str);
void hal_epd_get_info(hal_epd_info_t *out);

#ifdef __cplusplus
}
#endif
