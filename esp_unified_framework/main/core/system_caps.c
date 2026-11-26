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
#include "cJSON.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"
#include "app_config.h"
#include "board_pins.h"
#include "fleet_agent.h"
#include "ws_gateway.h"
#if CONFIG_FEATURE_DISPLAY
#include "hal_epd.h"
#endif

#ifdef CONFIG_APP_FW_VERSION
#define FW_VERSION_STR CONFIG_APP_FW_VERSION
#else
#define FW_VERSION_STR "1.1.0-unified"
#endif

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
    strlcpy(c->board_name, board_pins_get_name(), sizeof(c->board_name));
    strlcpy(c->fw_version, FW_VERSION_STR, sizeof(c->fw_version));

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    c->flash_size_mb = flash_size / (1024 * 1024);

    c->total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
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
#if CONFIG_FEATURE_PC_MONITOR
    c->feat_pc_monitor = true;
#endif
#if CONFIG_FEATURE_JOYSTICK
    c->feat_joystick = true;
#endif
#if CONFIG_FEATURE_FLEET_AGENT
    c->feat_fleet_agent = true;
#endif
#if CONFIG_FEATURE_SERIAL_CLI
    c->feat_serial_cli = true;
#endif
}

size_t system_caps_to_json(char *dst, size_t max_len)
{
    if (!dst || max_len < 32) return 0;

    system_caps_t c;
    system_caps_get(&c);

    app_config_t cfg;
    app_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "mdns_host", cfg.mdns_host);
    cJSON_AddStringToObject(root, "chip", c.chip_name);
    cJSON_AddStringToObject(root, "board", c.board_name);
    cJSON_AddStringToObject(root, "board_slug", board_pins_get_slug());
    cJSON_AddStringToObject(root, "fw_version", c.fw_version);
    cJSON_AddNumberToObject(root, "flash_mb", c.flash_size_mb);
    cJSON_AddStringToObject(root, "mac", c.mac_addr);
    cJSON_AddStringToObject(root, "ip", c.ip_addr);
    cJSON_AddNumberToObject(root, "web_port", cfg.web_port);
    cJSON_AddBoolToObject(root, "auth_required", cfg.api_token[0] != '\0');
    cJSON_AddBoolToObject(root, "ap_active", wifi_mgr_is_ap_active());
    cJSON_AddNumberToObject(root, "config_ver", cfg.config_ver);
    cJSON_AddStringToObject(root, "hub_url", cfg.hub_url);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", c.wifi_ssid);
    cJSON_AddNumberToObject(wifi, "rssi", c.wifi_rssi);
    cJSON_AddBoolToObject(wifi, "connected", wifi_mgr_connected());

    cJSON *mem = cJSON_AddObjectToObject(root, "memory");
    cJSON_AddNumberToObject(mem, "total_heap", c.total_heap);
    cJSON_AddNumberToObject(mem, "free_heap", c.free_heap);
    cJSON_AddNumberToObject(mem, "total_psram", c.total_psram);
    cJSON_AddNumberToObject(mem, "free_psram", c.free_psram);

    cJSON *caps = cJSON_AddObjectToObject(root, "capabilities");
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "http_server"), "enabled", c.feat_http_server);
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "web_ui"), "enabled", c.feat_web_ui);
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "rest_api"), "enabled", c.feat_rest_api);
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "gpio"), "enabled", c.feat_gpio);

    cJSON *cam = cJSON_AddObjectToObject(caps, "camera");
    cJSON_AddBoolToObject(cam, "enabled", c.feat_camera);
    cJSON_AddStringToObject(cam, "stream_url", "/stream");
    cJSON_AddStringToObject(cam, "capture_url", "/capture");

    cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "storage"), "enabled", c.feat_storage);
    cJSON *disp = cJSON_AddObjectToObject(caps, "display");
    cJSON_AddBoolToObject(disp, "enabled", c.feat_display);
    cJSON_AddBoolToObject(disp, "supported", c.feat_display || c.feat_pc_monitor);
#if CONFIG_FEATURE_PC_MONITOR
    cJSON_AddNumberToObject(disp, "width", 80);
    cJSON_AddNumberToObject(disp, "height", 160);
#elif CONFIG_FEATURE_DISPLAY
#if CONFIG_IDF_TARGET_ESP32S3
    bool is_epd = (strcmp(cfg.display.panel, "st7789_240") != 0);
#else
    bool is_epd = hal_epd_is_epd_panel_name(cfg.display.panel);
#endif
    if (is_epd) {
        hal_epd_info_t einfo;
        hal_epd_get_info(&einfo);
        cJSON_AddStringToObject(disp, "kind", "epd");
        cJSON_AddStringToObject(disp, "panel", einfo.panel_id ? einfo.panel_id : "epd_213_bwr");
        cJSON_AddNumberToObject(disp, "width", einfo.width ? einfo.width : 122);
        cJSON_AddNumberToObject(disp, "height", einfo.height ? einfo.height : 250);
    } else {
        cJSON_AddStringToObject(disp, "kind", "lcd");
        cJSON_AddStringToObject(disp, "panel", "st7789_240");
        cJSON_AddNumberToObject(disp, "width", 240);
        cJSON_AddNumberToObject(disp, "height", 240);
    }
#endif
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "usb_otg"), "enabled", c.feat_usb_otg);

    cJSON *pcmon = cJSON_AddObjectToObject(caps, "pc_monitor");
    cJSON_AddBoolToObject(pcmon, "enabled", c.feat_pc_monitor);
    cJSON_AddStringToObject(pcmon, "push_url", "/api/monitor/push");

    cJSON *joy = cJSON_AddObjectToObject(caps, "joystick");
    cJSON_AddBoolToObject(joy, "enabled", c.feat_joystick);
    cJSON_AddStringToObject(joy, "state_url", "/api/joystick/state");

    cJSON *ota = cJSON_AddObjectToObject(caps, "ota");
    cJSON_AddBoolToObject(ota, "enabled", c.feat_ota);
    cJSON_AddStringToObject(ota, "endpoint", "/api/ota/upload");

    cJSON *fa = cJSON_AddObjectToObject(caps, "fleet_agent");
    cJSON_AddBoolToObject(fa, "enabled", c.feat_fleet_agent);
    cJSON_AddBoolToObject(fa, "registered", fleet_agent_is_registered());
    char hub_url_buf[128] = "";
    fleet_agent_get_hub_url(hub_url_buf, sizeof(hub_url_buf));
    cJSON_AddStringToObject(fa, "active_hub", hub_url_buf);

    cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "serial_cli"), "enabled", c.feat_serial_cli);

    cJSON *wsg = cJSON_AddObjectToObject(caps, "ws_gateway");
#if CONFIG_FEATURE_WS_GATEWAY
    cJSON_AddBoolToObject(wsg, "enabled", true);
    cJSON_AddBoolToObject(wsg, "connected", ws_gateway_is_online());
#else
    cJSON_AddBoolToObject(wsg, "enabled", false);
    cJSON_AddBoolToObject(wsg, "connected", false);
#endif

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        dst[0] = '\0';
        return 0;
    }
    size_t len = strlcpy(dst, printed, max_len);
    cJSON_free(printed);
    return (len < max_len) ? len : (max_len - 1);
}

size_t system_status_to_json(char *dst, size_t max_len)
{
    if (!dst || max_len < 32) return 0;

    system_caps_t c;
    system_caps_get(&c);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "uptime_sec", c.uptime_sec);
    cJSON_AddNumberToObject(root, "free_heap", c.free_heap);
    cJSON_AddNumberToObject(root, "free_psram", c.free_psram);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_mgr_connected());
    cJSON_AddNumberToObject(wifi, "rssi", c.wifi_rssi);
    cJSON_AddStringToObject(wifi, "ip", c.ip_addr);
    cJSON_AddStringToObject(wifi, "ssid", c.wifi_ssid);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        dst[0] = '\0';
        return 0;
    }
    size_t len = strlcpy(dst, printed, max_len);
    cJSON_free(printed);
    return (len < max_len) ? len : (max_len - 1);
}
