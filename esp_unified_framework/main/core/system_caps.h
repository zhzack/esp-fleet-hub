#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char chip_name[48];
    char board_name[40];
    char fw_version[32];
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
    bool feat_http_server;
    bool feat_web_ui;
    bool feat_rest_api;
    bool feat_gpio;
    bool feat_camera;
    bool feat_storage;
    bool feat_display;
    bool feat_usb_otg;
    bool feat_ota;
    bool feat_pc_monitor;
    bool feat_joystick;
    bool feat_fleet_agent;
    bool feat_serial_cli;
} system_caps_t;

esp_err_t system_caps_init(void);
void system_caps_get(system_caps_t *out_caps);
size_t system_caps_to_json(char *dst, size_t max_len);
size_t system_status_to_json(char *dst, size_t max_len);

#ifdef __cplusplus
}
#endif
