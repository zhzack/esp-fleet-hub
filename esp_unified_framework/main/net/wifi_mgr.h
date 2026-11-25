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
bool wifi_mgr_is_ap_active(void);
int wifi_mgr_rssi(void);
void wifi_mgr_get_ip(char *buf, size_t len);
void wifi_mgr_get_ssid(char *buf, size_t len);

/**
 * @brief Perform 2.4GHz WiFi scan and format results as JSON object
 */
size_t wifi_mgr_scan(char *json_out, size_t max_len);

/**
 * @brief Safely switch STA to new WiFi credentials with 15s automatic rollback on failure
 */
esp_err_t wifi_mgr_apply_new_sta(const char *ssid, const char *pass);

/**
 * @brief Launch SoftAP + Captive Portal DNS server (ESP-Setup-XXXX @ 192.168.4.1)
 */
esp_err_t wifi_mgr_start_softap(void);

/**
 * @brief Tear down SoftAP setup portal and return to STA mode
 */
esp_err_t wifi_mgr_stop_softap(void);

/**
 * @brief Update mDNS hostname/instance dynamically without reboot
 */
void wifi_mgr_update_mdns(const char *hostname, const char *instance_name, uint16_t port);

#ifdef __cplusplus
}
#endif
