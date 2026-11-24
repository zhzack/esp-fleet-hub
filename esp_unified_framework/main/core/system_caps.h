#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char chip_name[32];
    uint32_t cpu_freq_mhz;
    uint32_t flash_size_mb;
    uint32_t total_heap;
    uint32_t free_heap;
    uint32_t total_psram;
    uint32_t free_psram;
    uint32_t uptime_sec;
    int wifi_rssi;
    char wifi_ssid[33];
    char ip_addr[16];
    char mac_addr[18];

    // Capabilities
    bool feat_gpio;
    bool feat_camera;
    bool feat_storage;
    bool feat_display;
    bool feat_usb_otg;
    bool feat_ota;
} system_caps_t;

/**
 * @brief Initialize capabilities query engine
 */
esp_err_t system_caps_init(void);

/**
 * @brief Snapshot current system state & capabilities
 */
void system_caps_get(system_caps_t *out_caps);

/**
 * @brief Generate JSON payload describing static & dynamic hardware capabilities
 * @return String length written to dst
 */
size_t system_caps_to_json(char *dst, size_t max_len);

/**
 * @brief Generate JSON payload describing live performance status (RAM, PSRAM, Uptime, WiFi)
 */
size_t system_status_to_json(char *dst, size_t max_len);

#ifdef __cplusplus
}
#endif
