#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char device_id[33];
    char mdns_host[33];
    uint16_t web_port;
} app_config_t;

esp_err_t app_config_init(void);
esp_err_t app_config_load(app_config_t *out_cfg);
esp_err_t app_config_save(const app_config_t *in_cfg);
void app_config_get(app_config_t *out_cfg);

#ifdef __cplusplus
}
#endif
