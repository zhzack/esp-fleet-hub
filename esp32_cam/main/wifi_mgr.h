#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_MAX 20

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t auth;   /* wifi_auth_mode_t */
} wifi_scan_entry_t;

/** One-shot init: netif + event loop + wifi driver. Call once. */
esp_err_t wifi_mgr_init(void);

/** Bring up STA and try to connect. Blocks up to timeout_ms.
 *  Returns ESP_OK on first GOT_IP, else ESP_FAIL. */
esp_err_t wifi_mgr_sta_connect(const char *ssid, const char *pass, int timeout_ms);

/** Disconnect & stop STA (keeps driver running). */
void wifi_mgr_sta_stop(void);

/** Start a SoftAP (open network). Coexists with STA when set after sta_connect. */
esp_err_t wifi_mgr_ap_start(const char *ap_ssid);

/** Stop SoftAP. */
void wifi_mgr_ap_stop(void);

/** Synchronous scan. Fills out_arr (capacity WIFI_SCAN_MAX), writes count to *out_n. */
esp_err_t wifi_mgr_scan(wifi_scan_entry_t *out_arr, int *out_n);

/** Convenience used by ble_hid_main: init + sta_connect. */
esp_err_t wifi_mgr_start(const char *ssid, const char *pass, int timeout_ms);

int  wifi_mgr_rssi(void);
bool wifi_mgr_connected(void);
bool wifi_mgr_get_ip(char *buf, int buflen);   /* writes "a.b.c.d" or "" */
bool wifi_mgr_get_ssid(char *buf, int buflen);

#ifdef __cplusplus
}
#endif
