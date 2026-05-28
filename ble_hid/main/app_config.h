#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_DEVICE_ID_LEN   32
#define APP_TOKEN_LEN       192
#define APP_HOST_LEN        64
#define APP_SSID_LEN        33
#define APP_PASS_LEN        65

typedef struct {
    char     wifi_ssid[APP_SSID_LEN];
    char     wifi_pass[APP_PASS_LEN];
    char     server_host[APP_HOST_LEN];
    uint16_t server_port;
    char     device_id[APP_DEVICE_ID_LEN];
    char     device_token[APP_TOKEN_LEN];
    bool     strict_seq;
    uint16_t queue_size;
    uint8_t  queue_policy;   /* 0 = drop_oldest, 1 = reject_new */
} app_config_t;

/** Read config from NVS namespace "app". Missing keys fall back to compile-time defaults. */
esp_err_t app_config_load(app_config_t *cfg);

/** Persist config back to NVS. */
esp_err_t app_config_save(const app_config_t *cfg);

/** Pointer to the running config snapshot (loaded once at boot). */
const app_config_t *app_config_get(void);

#ifdef __cplusplus
}
#endif
