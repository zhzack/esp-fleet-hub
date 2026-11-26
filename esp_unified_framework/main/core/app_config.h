#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_DISPLAY_MAX_LINES 6

typedef struct {
    char mode[16];         // "monitor" | "text" | "status" | "off"
    uint8_t backlight;     // 0-100
    char theme[16];        // "cyber" | "emerald" | "amber" | "mono" | "paper"
    char panel[24];        // "epd_213_bw" | "epd_213_bwr" | "epd_420_bwr" | "st7789_240" | "st7735_160" | "auto"
    uint16_t rotation;     // 0 | 90 | 180 | 270
    bool invert;           // Invert black/white ink
    bool partial_refresh;  // Fast partial update when supported (epd_213_bw)
    uint16_t refresh_sec;  // Periodic auto-refresh seconds for EPD status/monitor (0 = manual/on-change)
    uint8_t red_mask;      // Bitmask: bits 0..5 -> lines[0..5] in RED, bit 6 -> header, bit 7 -> footer
    int8_t pin_mosi;       // -1 = board default
    int8_t pin_clk;
    int8_t pin_cs;
    int8_t pin_dc;
    int8_t pin_rst;
    int8_t pin_busy;
    char header_text[24];  // Custom top bar label (empty = auto)
    char footer_text[24];  // Custom footer badge (empty = auto)
    bool show_clock;
    bool show_cpu;
    bool show_ram;
    bool show_disk;
    bool show_net;
    bool show_uptime;
    char lines[APP_DISPLAY_MAX_LINES][40];
} app_display_config_t;

typedef struct {
    // Network & WiFi
    char wifi_ssid[33];
    char wifi_pass[65];
    char wifi_ssid_bak[33];
    char wifi_pass_bak[65];
    char device_id[33];
    char mdns_host[33];
    uint16_t web_port;

    // Fleet Hub Registration & Auth
    char hub_url[96];
    char ws_gateway_url[96];
    char device_secret[65];
    char api_token[33];
    uint32_t config_ver;

    // Camera Runtime Parameters
    uint8_t cam_framesize;  // default 8 (FRAMESIZE_VGA)
    uint8_t cam_quality;    // 4-63, default 12
    bool cam_vflip;         // default true
    bool cam_hmirror;       // default false
    int8_t cam_brightness;  // -2..2, default 1

    // Custom GPIO Overrides JSON (stored as separate NVS string "gpio_cfg")
    char gpio_cfg_json[512];

    // Display Configuration (persisted as NVS string "disp_cfg")
    app_display_config_t display;
} app_config_t;

typedef struct {
    bool wifi_changed;
    bool mdns_changed;
    bool port_changed;
    bool camera_changed;
    bool gpio_changed;
    bool auth_changed;
    bool display_changed;
} app_config_changes_t;

esp_err_t app_config_init(void);
esp_err_t app_config_load(app_config_t *out_cfg);
esp_err_t app_config_save(const app_config_t *in_cfg);
void app_config_get(app_config_t *out_cfg);
const app_config_t *app_config_ptr(void);

/**
 * @brief Generate safe JSON summary (never exposes plaintext secrets)
 */
size_t app_config_to_json(char *dst, size_t max_len);

/**
 * @brief Parse JSON patch and update NVS config, returning which subsystems need live reload
 */
esp_err_t app_config_update_from_json_ex(const char *json_str, app_config_changes_t *out_changes);
esp_err_t app_config_update_from_json(const char *json_str);

/**
 * @brief Save working WiFi credentials to backup slot before switching to a new WiFi
 */
esp_err_t app_config_backup_wifi(const char *ssid, const char *pass);

/**
 * @brief Restore WiFi credentials from backup slot
 */
esp_err_t app_config_rollback_wifi(void);

/**
 * @brief Persist updated gpio_cfg JSON into NVS
 */
esp_err_t app_config_save_gpio_json(const char *json_str);

/**
 * @brief Parse display JSON object into updated->display, returning true if changed
 */
bool app_config_apply_display_json(const char *json_str, app_config_t *updated, bool switch_text_on_lines);

/**
 * @brief Populate cJSON object with serialized display configuration
 */
void app_config_display_to_cjson(const app_display_config_t *disp, void *cjson_obj);

/**
 * @brief Wipe NVS "app" namespace and reset defaults
 */
esp_err_t app_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
