#include "wifi_mgr.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mdns.h"
#include "app_config.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static bool s_connected = false;
static int s_retry_count = 0;
static char s_current_ip[16] = "0.0.0.0";
static char s_current_ssid[33] = "";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        strncpy(s_current_ip, "0.0.0.0", sizeof(s_current_ip));
        s_retry_count++;
        ESP_LOGW(TAG, "WiFi disconnected (attempt %d), reconnecting in 2s...", s_retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_current_ip, sizeof(s_current_ip));
        s_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "WiFi Connected! IP: %s", s_current_ip);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Start mDNS
        app_config_t cfg;
        app_config_get(&cfg);
        mdns_init();
        mdns_hostname_set(cfg.mdns_host);
        mdns_instance_name_set(cfg.device_id);
        mdns_service_add(NULL, "_http", "_tcp", cfg.web_port, NULL, 0);
        ESP_LOGI(TAG, "mDNS responder started: http://%s.local", cfg.mdns_host);
    }
}

esp_err_t wifi_mgr_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    return ESP_OK;
}

esp_err_t wifi_mgr_start(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    strncpy(s_current_ssid, cfg.wifi_ssid, sizeof(s_current_ssid) - 1);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, cfg.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, cfg.wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: [%s]", cfg.wifi_ssid);
    return ESP_OK;
}

bool wifi_mgr_connected(void)
{
    return s_connected;
}

int wifi_mgr_rssi(void)
{
    if (!s_connected) return -100;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -100;
}

void wifi_mgr_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_current_ip, len - 1);
    buf[len - 1] = '\0';
}

void wifi_mgr_get_ssid(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_current_ssid, len - 1);
    buf[len - 1] = '\0';
}

size_t wifi_mgr_scan(char *json_out, size_t max_len)
{
    if (!json_out || max_len < 16) return 0;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE
    };

    esp_wifi_scan_start(&scan_config, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 16) ap_count = 16;

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list) {
        return snprintf(json_out, max_len, "{\"ok\":false,\"error\":\"no_mem\"}");
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    size_t offset = 0;
    offset += snprintf(json_out + offset, max_len - offset, "{\"ok\":true,\"aps\":[");

    for (int i = 0; i < ap_count; i++) {
        if (offset >= max_len - 64) break;
        offset += snprintf(json_out + offset, max_len - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                           (i == 0) ? "" : ",",
                           (char *)ap_list[i].ssid,
                           ap_list[i].rssi,
                           ap_list[i].authmode);
    }

    free(ap_list);
    offset += snprintf(json_out + offset, max_len - offset, "]}");
    return offset;
}
