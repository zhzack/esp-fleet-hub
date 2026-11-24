#include "system_caps.h"

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"
#include "app_config.h"

static const char *get_chip_model_name(esp_chip_model_t model)
{
    switch (model) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        case CHIP_ESP32C2: return "ESP32-C2";
        case CHIP_ESP32C6: return "ESP32-C6";
        case CHIP_ESP32H2: return "ESP32-H2";
        default:           return "ESP32-Generic";
    }
}

esp_err_t system_caps_init(void)
{
    return ESP_OK;
}

void system_caps_get(system_caps_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    snprintf(c->chip_name, sizeof(c->chip_name), "%s (rev v%d.%d, %d cores)",
             get_chip_model_name(chip_info.model),
             chip_info.revision / 100, chip_info.revision % 100,
             chip_info.cores);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    c->flash_size_mb = flash_size / (1024 * 1024);

    c->total_heap = esp_get_free_heap_size(); // approximate total
    c->free_heap = esp_get_free_heap_size();

#if CONFIG_SPIRAM
    c->total_psram = esp_psram_get_size();
    c->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    c->total_psram = 0;
    c->free_psram = 0;
#endif

    c->uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(c->mac_addr, sizeof(c->mac_addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    wifi_mgr_get_ip(c->ip_addr, sizeof(c->ip_addr));
    wifi_mgr_get_ssid(c->wifi_ssid, sizeof(c->wifi_ssid));
    c->wifi_rssi = wifi_mgr_rssi();

    // Feature Flags (evaluated at compile-time via Kconfig)
#if CONFIG_FEATURE_HTTP_SERVER
    c->feat_http_server = true;
#endif
#if CONFIG_FEATURE_WEB_UI
    c->feat_web_ui = true;
#endif
#if CONFIG_FEATURE_REST_API
    c->feat_rest_api = true;
#endif
#if CONFIG_FEATURE_GPIO_CTRL
    c->feat_gpio = true;
#endif
#if CONFIG_FEATURE_CAMERA
    c->feat_camera = true;
#endif
#if CONFIG_FEATURE_STORAGE
    c->feat_storage = true;
#endif
#if CONFIG_FEATURE_DISPLAY
    c->feat_display = true;
#endif
#if CONFIG_FEATURE_USB_OTG
    c->feat_usb_otg = true;
#endif
#if CONFIG_FEATURE_OTA
    c->feat_ota = true;
#endif
}

size_t system_caps_to_json(char *dst, size_t max_len)
{
    system_caps_t c;
    system_caps_get(&c);

    app_config_t cfg;
    app_config_get(&cfg);

    return snprintf(dst, max_len,
        "{"
          "\"ok\":true,"
          "\"device_id\":\"%s\","
          "\"chip\":\"%s\","
          "\"flash_mb\":%lu,"
          "\"mac\":\"%s\","
          "\"ip\":\"%s\","
          "\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"connected\":%s},"
          "\"memory\":{"
            "\"free_heap\":%lu,"
            "\"total_psram\":%lu,"
            "\"free_psram\":%lu"
          "},"
          "\"capabilities\":{"
            "\"http_server\":{\"enabled\":%s},"
            "\"web_ui\":{\"enabled\":%s},"
            "\"rest_api\":{\"enabled\":%s},"
            "\"gpio\":{\"enabled\":%s},"
            "\"camera\":{\"enabled\":%s,\"stream_url\":\"/stream\",\"capture_url\":\"/capture\"},"
            "\"storage\":{\"enabled\":%s},"
            "\"display\":{\"enabled\":%s},"
            "\"usb_otg\":{\"enabled\":%s},"
            "\"ota\":{\"enabled\":%s,\"endpoint\":\"/api/ota/upload\"}"
          "}"
        "}",
        cfg.device_id, c.chip_name, c.flash_size_mb, c.mac_addr, c.ip_addr,
        c.wifi_ssid, c.wifi_rssi, wifi_mgr_connected() ? "true" : "false",
        (unsigned long)c.free_heap, (unsigned long)c.total_psram, (unsigned long)c.free_psram,
        c.feat_http_server ? "true" : "false",
        c.feat_web_ui ? "true" : "false",
        c.feat_rest_api ? "true" : "false",
        c.feat_gpio ? "true" : "false",
        c.feat_camera ? "true" : "false",
        c.feat_storage ? "true" : "false",
        c.feat_display ? "true" : "false",
        c.feat_usb_otg ? "true" : "false",
        c.feat_ota ? "true" : "false"
    );
}

size_t system_status_to_json(char *dst, size_t max_len)
{
    system_caps_t c;
    system_caps_get(&c);

    return snprintf(dst, max_len,
        "{"
          "\"ok\":true,"
          "\"uptime_sec\":%lu,"
          "\"free_heap\":%lu,"
          "\"free_psram\":%lu,"
          "\"wifi\":{\"connected\":%s,\"rssi\":%d,\"ip\":\"%s\"}"
        "}",
        (unsigned long)c.uptime_sec,
        (unsigned long)c.free_heap,
        (unsigned long)c.free_psram,
        wifi_mgr_connected() ? "true" : "false",
        c.wifi_rssi,
        c.ip_addr
    );
}

