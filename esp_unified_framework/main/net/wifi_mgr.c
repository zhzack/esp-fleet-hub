#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "board_pins.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static volatile bool s_connected = false;
static volatile bool s_ap_active = false;
static volatile bool s_mdns_inited = false;
static volatile bool s_mdns_services_added = false;
static volatile bool s_testing_new_wifi = false;
static volatile int s_retry_count = 0;
static char s_current_ip[16] = "0.0.0.0";
static char s_current_ssid[33] = "";

static esp_timer_handle_t s_rollback_timer = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static TaskHandle_t s_dns_task_handle = NULL;
static int s_dns_sock = -1;

static void reconnect_timer_cb(void *arg)
{
    if (!s_connected) {
        esp_wifi_connect();
    }
}

/* ================= Captive Portal DNS Server (UDP Port 53) ================= */
static void captive_dns_task(void *pvParameters)
{
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_dns_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(s_dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno);
        close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive Portal DNS responder active on UDP:53 -> 192.168.4.1");

    uint8_t rx_buf[256];
    uint8_t tx_buf[300];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (s_ap_active) {
        int len = recvfrom(s_dns_sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&client_addr, &client_len);
        if (len < 12) continue;
        if (len > 220) continue;

        memcpy(tx_buf, rx_buf, len);
        // Set QR=1 (Response), AA=1 (Authoritative), RCODE=0
        tx_buf[2] = 0x84;
        tx_buf[3] = 0x00;
        // Answer count = 1 (ANCOUNT)
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x01;
        // NSCOUNT = 0, ARCOUNT = 0
        tx_buf[8] = 0x00;
        tx_buf[9] = 0x00;
        tx_buf[10] = 0x00;
        tx_buf[11] = 0x00;

        // Append Answer RR: Name pointer 0xC00C, Type A (1), Class IN (1), TTL 60s, RDLEN 4, IP 192.168.4.1
        uint8_t ans[16] = {
            0xC0, 0x0C,
            0x00, 0x01,
            0x00, 0x01,
            0x00, 0x00, 0x00, 0x3C,
            0x00, 0x04,
            192, 168, 4, 1
        };
        memcpy(tx_buf + len, ans, sizeof(ans));
        sendto(s_dns_sock, tx_buf, len + sizeof(ans), 0, (struct sockaddr *)&client_addr, client_len);
    }

    close(s_dns_sock);
    s_dns_sock = -1;
    s_dns_task_handle = NULL;
    ESP_LOGI(TAG, "Captive Portal DNS responder stopped");
    vTaskDelete(NULL);
}

static void wifi_rollback_timer_cb(void *arg)
{
    if (!s_testing_new_wifi) return;
    s_testing_new_wifi = false;

    if (s_connected) {
        return; // Connected in time!
    }

    ESP_LOGW(TAG, "New WiFi failed to connect within 15s! Triggering automatic rollback...");
    if (app_config_rollback_wifi() == ESP_OK) {
        const app_config_t *cfg = app_config_ptr();
        strlcpy(s_current_ssid, cfg->wifi_ssid, sizeof(s_current_ssid));

        wifi_config_t wifi_cfg = {0};
        strlcpy((char *)wifi_cfg.sta.ssid, cfg->wifi_ssid, sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, cfg->wifi_pass, sizeof(wifi_cfg.sta.password));
        wifi_cfg.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_connect();
    } else {
        wifi_mgr_start_softap();
    }
}

static void build_mdns_caps_str(char *dst, size_t max_len)
{
    dst[0] = '\0';
    struct { bool on; const char *tag; } items[] = {
#if CONFIG_FEATURE_USB_OTG
        { true, "usb_hid" },
#endif
#if CONFIG_FEATURE_DISPLAY
        { true, "display" },
#endif
#if CONFIG_FEATURE_PC_MONITOR
        { true, "pc_monitor" },
#endif
#if CONFIG_FEATURE_GPIO_CTRL
        { true, "gpio" },
#endif
#if CONFIG_FEATURE_CAMERA
        { true, "camera" },
#endif
#if CONFIG_FEATURE_STORAGE
        { true, "storage" },
#endif
#if CONFIG_FEATURE_OTA
        { true, "ota" },
#endif
#if CONFIG_FEATURE_WS_GATEWAY
        { true, "ws_client" },
#endif
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (!items[i].on) continue;
        if (dst[0] != '\0') strlcat(dst, ",", max_len);
        strlcat(dst, items[i].tag, max_len);
    }
}

void wifi_mgr_update_mdns(const char *hostname, const char *instance_name, uint16_t port)
{
    const app_config_t *cfg = app_config_ptr();
    uint16_t effective_port = port ? port : (cfg->web_port ? cfg->web_port : 80);
    const char *eff_host = (hostname && hostname[0]) ? hostname : (cfg->mdns_host[0] ? cfg->mdns_host : "esp-node");
    const char *eff_inst = (instance_name && instance_name[0]) ? instance_name : (cfg->device_id[0] ? cfg->device_id : eff_host);

    if (!s_mdns_inited) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
            return;
        }
        s_mdns_inited = true;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(eff_host));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_instance_name_set(eff_inst));

    char caps_buf[128] = "";
    char port_buf[8] = "";
    build_mdns_caps_str(caps_buf, sizeof(caps_buf));
    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)effective_port);
    const char *auth_flag = (cfg->api_token[0] != '\0' || (cfg->auth_strict && cfg->device_secret[0] != '\0')) ? "1" : "0";
    mdns_txt_item_t txt[] = {
        { "id", cfg->device_id },
        { "mdns", cfg->mdns_host },
        { "slug", board_pins_get_slug() },
#ifdef CONFIG_APP_FW_VERSION
        { "fw", CONFIG_APP_FW_VERSION },
#else
        { "fw", "1.1.0-unified" },
#endif
        { "auth", auth_flag },
        { "strict", cfg->auth_strict ? "1" : "0" },
        { "caps", caps_buf },
        { "port", port_buf },
    };

    if (!s_mdns_services_added) {
        esp_err_t e_http = mdns_service_add(eff_inst, "_http", "_tcp", effective_port, NULL, 0);
        esp_err_t e_uni = mdns_service_add(
            eff_inst, "_esp-unified", "_tcp", effective_port, txt, sizeof(txt) / sizeof(txt[0]));
        if (e_http != ESP_OK) {
            ESP_LOGW(TAG, "mdns _http add failed: %s", esp_err_to_name(e_http));
        }
        if (e_uni != ESP_OK) {
            ESP_LOGW(TAG, "mdns _esp-unified add failed: %s", esp_err_to_name(e_uni));
        } else {
            s_mdns_services_added = true;
        }
    } else {
        esp_err_t e_txt = mdns_service_txt_set(
            "_esp-unified", "_tcp", txt, sizeof(txt) / sizeof(txt[0]));
        if (e_txt != ESP_OK) {
            ESP_LOGW(TAG, "mdns _esp-unified txt_set failed: %s", esp_err_to_name(e_txt));
        }
    }

    ESP_LOGI(TAG, "mDNS active: http://%s.local (_esp-unified._tcp, auth=%s, caps=%s)",
             eff_host, auth_flag, caps_buf);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        strlcpy(s_current_ip, "0.0.0.0", sizeof(s_current_ip));
        s_retry_count++;
        ESP_LOGW(TAG, "WiFi disconnected (attempt %d)...", s_retry_count);

        uint8_t thr = app_config_ptr()->wifi_ap_fail_threshold;
        if (thr < 1) thr = 5;
        if (!s_ap_active && !s_testing_new_wifi && s_retry_count >= thr) {
            ESP_LOGW(TAG, "STA connection failed %d times — starting SoftAP Captive Portal!", s_retry_count);
            wifi_mgr_start_softap();
        }
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)(s_ap_active ? 8000 : 2000) * 1000ULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_current_ip, sizeof(s_current_ip));
        s_connected = true;
        s_retry_count = 0;

        const app_config_t *cfg = app_config_ptr();
        if (s_testing_new_wifi) {
            s_testing_new_wifi = false;
            if (s_rollback_timer) {
                esp_timer_stop(s_rollback_timer);
            }
            app_config_backup_wifi(cfg->wifi_ssid, cfg->wifi_pass);
            ESP_LOGI(TAG, "New WiFi validated & promoted to backup!");
        }

        ESP_LOGI(TAG, "WiFi Connected! IP: %s", s_current_ip);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        wifi_mgr_update_mdns(cfg->mdns_host, cfg->device_id, cfg->web_port);

        if (s_ap_active) {
            ESP_LOGI(TAG, "STA recovered while SoftAP active.");
        }
    }
}

esp_err_t wifi_mgr_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

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

    const esp_timer_create_args_t timer_args = {
        .callback = &wifi_rollback_timer_cb,
        .name = "wifi_rollback"
    };
    esp_timer_create(&timer_args, &s_rollback_timer);

    const esp_timer_create_args_t rec_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconn"
    };
    esp_timer_create(&rec_args, &s_reconnect_timer);
    return ESP_OK;
}

esp_err_t wifi_mgr_start(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    strlcpy(s_current_ssid, cfg.wifi_ssid, sizeof(s_current_ssid));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, cfg.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, cfg.wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = cfg.wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: [%s]", cfg.wifi_ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_start_softap(void)
{
    if (s_ap_active) return ESP_OK;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    wifi_config_t ap_cfg = {0};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "ESP-Setup-%02X%02X", mac[4], mac[5]);
    ap_cfg.ap.ssid_len = strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    s_ap_active = true;

    if (!s_dns_task_handle) {
        xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 3, &s_dns_task_handle);
    }

    ESP_LOGW(TAG, "SoftAP Started! Connect phone/PC to WiFi '%s' -> http://192.168.4.1/", ap_cfg.ap.ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_stop_softap(void)
{
    if (!s_ap_active) return ESP_OK;
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "SoftAP Captive Portal stopped (switched back to STA)");
    return ESP_OK;
}

esp_err_t wifi_mgr_apply_new_sta(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
    s_connected = false;
    s_retry_count = 0;
    s_testing_new_wifi = true;

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (pass) strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    if (s_rollback_timer) {
        esp_timer_stop(s_rollback_timer);
        esp_timer_start_once(s_rollback_timer, 15 * 1000000ULL);
    }
    ESP_LOGI(TAG, "Switching STA to '%s' (15s rollback protection active)...", ssid);
    return ESP_OK;
}

bool wifi_mgr_connected(void)
{
    return s_connected;
}

bool wifi_mgr_is_ap_active(void)
{
    return s_ap_active;
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
    if (s_connected) {
        strlcpy(buf, s_current_ip, len);
    } else if (s_ap_active) {
        strlcpy(buf, "192.168.4.1", len);
    } else {
        strlcpy(buf, "0.0.0.0", len);
    }
}

void wifi_mgr_get_ssid(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strlcpy(buf, s_current_ssid, len);
}

size_t wifi_mgr_scan(char *json_out, size_t max_len)
{
    if (!json_out || max_len < 32) return 0;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        return snprintf(json_out, max_len, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 16) ap_count = 16;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *aps = cJSON_AddArrayToObject(root, "aps");

    if (ap_count > 0) {
        wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (!ap_list) {
            cJSON_Delete(root);
            return snprintf(json_out, max_len, "{\"ok\":false,\"error\":\"no_mem\"}");
        }
        if (esp_wifi_scan_get_ap_records(&ap_count, ap_list) == ESP_OK) {
            for (int i = 0; i < ap_count; i++) {
                if (ap_list[i].ssid[0] == '\0') continue;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "ssid", (const char *)ap_list[i].ssid);
                cJSON_AddNumberToObject(item, "rssi", ap_list[i].rssi);
                cJSON_AddNumberToObject(item, "auth", ap_list[i].authmode);
                cJSON_AddItemToArray(aps, item);
            }
        }
        free(ap_list);
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return snprintf(json_out, max_len, "{\"ok\":false,\"error\":\"json_fmt\"}");
    }
    size_t len = strlcpy(json_out, printed, max_len);
    cJSON_free(printed);
    return (len < max_len) ? len : (max_len - 1);
}
