#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_mgr_init(void);
esp_err_t wifi_mgr_start(void);

bool wifi_mgr_connected(void);
int wifi_mgr_rssi(void);
void wifi_mgr_get_ip(char *buf, size_t len);
void wifi_mgr_get_ssid(char *buf, size_t len);

/**
 * @brief Perform 2.4GHz WiFi scan and format results as JSON array
 */
size_t wifi_mgr_scan(char *json_out, size_t max_len);

#ifdef __cplusplus
}
#endif
