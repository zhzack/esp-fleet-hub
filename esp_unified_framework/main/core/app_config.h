#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
} app_config_t;

typedef struct {
    bool wifi_changed;
    bool mdns_changed;
    bool port_changed;
    bool camera_changed;
    bool gpio_changed;
    bool auth_changed;
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
 * @brief Wipe NVS "app" namespace and reset defaults
 */
esp_err_t app_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
