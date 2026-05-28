#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start SoftAP + HTTP server for first-time WiFi provisioning.
 *  Blocks the calling task until user submits credentials and they verify,
 *  or returns ESP_ERR_TIMEOUT after timeout_ms (0 = wait forever).
 *  On success the new ssid/pass are persisted to NVS via app_config_save(),
 *  and the AP is taken down. */
esp_err_t provision_run(int timeout_ms);

#ifdef __cplusplus
}
#endif
