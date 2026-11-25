#include "wifi_mgr.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_eg;
#define BIT_CONNECTED  BIT0
#define BIT_FAIL       BIT1

static int  s_retry = 0;
static int  s_max_retry = 10;
static bool s_connected = false;
static bool s_inited = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static char s_cur_ssid[33] = {0};
static char s_cur_pass[65] = {0};
static char s_cur_ip[16]   = {0};

static const char *wifi_disconn_reason_str(uint8_t reason)
{
    switch (reason) {
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
        default:  return "OTHER";
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "WiFi station started, initiating connection...");
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "WiFi connected to AP successfully, obtaining IP...");
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)data;
            uint8_t reason = disconn ? disconn->reason : 0;
            s_connected = false;
            s_cur_ip[0] = 0;
            ESP_LOGW(TAG, "WiFi disconnected, reason=%d (%s)", reason, wifi_disconn_reason_str(reason));
            if (s_cur_ssid[0] && s_retry < s_max_retry) {
                s_retry++;
                ESP_LOGW(TAG, "WiFi reconnecting %d/%d...", s_retry, s_max_retry);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_eg, BIT_FAIL);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_cur_ip, sizeof s_cur_ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "=============================================");
        ESP_LOGI(TAG, "🎉 WiFi Got IP: %s", s_cur_ip);
        ESP_LOGI(TAG, "=============================================");
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
    }
}

esp_err_t wifi_mgr_init(void)
{
    if (s_inited) return ESP_OK;
    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL));

    /* default mode = STA; ap_start() will flip to APSTA when needed */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_mgr_sta_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!s_inited) wifi_mgr_init();
    s_retry = 0;
    s_connected = false;
    xEventGroupClearBits(s_eg, BIT_CONNECTED | BIT_FAIL);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    if (pass) strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* allow open + WPA2 */
    wc.sta.pmf_cfg.capable = true;

    /* if AP is also running keep APSTA, else STA */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) mode = WIFI_MODE_APSTA;
    else if (mode != WIFI_MODE_APSTA) mode = WIFI_MODE_STA;
    esp_wifi_set_mode(mode);

    strlcpy(s_cur_ssid, ssid, sizeof s_cur_ssid);
    if (pass) strlcpy(s_cur_pass, pass, sizeof s_cur_pass);

    /* must be idle before set_config: tear down any in-flight connect first */
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Calling esp_wifi_connect() for SSID: '%s'...", ssid);
    esp_err_t conn_err = esp_wifi_connect();
    if (conn_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect returned error: %s", esp_err_to_name(conn_err));
    }

    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_CONNECTED | BIT_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGW(TAG, "Initial connect timed out or failed. Scanning available 2.4GHz WiFi networks...");
        wifi_scan_entry_t entries[WIFI_SCAN_MAX];
        int n_found = 0;
        wifi_mgr_scan(entries, &n_found);
    }
    return (bits & BIT_CONNECTED) ? ESP_OK : ESP_FAIL;
}

void wifi_mgr_sta_stop(void)
{
    s_connected = false;
    s_cur_ip[0] = 0;
    esp_wifi_disconnect();
}

esp_err_t wifi_mgr_ap_start(const char *ap_ssid)
{
    if (!s_inited) wifi_mgr_init();
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_STA) mode = WIFI_MODE_APSTA;
    else mode = WIFI_MODE_AP;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    wifi_config_t ac = { 0 };
    strlcpy((char *)ac.ap.ssid, ap_ssid, sizeof ac.ap.ssid);
    ac.ap.ssid_len = strlen(ap_ssid);
    ac.ap.channel = 1;
    ac.ap.max_connection = 4;
    ac.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ac));
    ESP_LOGI(TAG, "SoftAP up: %s", ap_ssid);
    return ESP_OK;
}

void wifi_mgr_ap_stop(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA) esp_wifi_set_mode(WIFI_MODE_STA);
    else if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_NULL);
}

esp_err_t wifi_mgr_scan(wifi_scan_entry_t *out_arr, int *out_n)
{
    if (!s_inited) wifi_mgr_init();

    /* 注意：BT 启用时 IDF 不允许自定义 active dwell time，必须用默认。
     * show_hidden 仍可设。 */
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_LOGI(TAG, "scan start (active, show_hidden=1)");
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        *out_n = 0;
        return err;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "scan done: %u APs reported", n);
    if (n > WIFI_SCAN_MAX) n = WIFI_SCAN_MAX;
    if (n == 0) { *out_n = 0; return ESP_OK; }

    wifi_ap_record_t *recs = malloc((size_t)n * sizeof(wifi_ap_record_t));
    if (!recs) { *out_n = 0; return ESP_ERR_NO_MEM; }
    esp_wifi_scan_get_ap_records(&n, recs);

    for (int i = 0; i < n; ++i) {
        strlcpy(out_arr[i].ssid, (const char *)recs[i].ssid, sizeof out_arr[i].ssid);
        out_arr[i].rssi = recs[i].rssi;
        out_arr[i].auth = recs[i].authmode;
        ESP_LOGI(TAG, "  [%d] %-32s rssi=%d auth=%d ch=%d",
                 i, out_arr[i].ssid, out_arr[i].rssi, out_arr[i].auth, recs[i].primary);
    }
    free(recs);
    *out_n = n;
    return ESP_OK;
}

esp_err_t wifi_mgr_start(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_mgr_init();
    return wifi_mgr_sta_connect(ssid, pass, timeout_ms);
}

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

bool wifi_mgr_connected(void) { return s_connected; }

bool wifi_mgr_get_ip(char *buf, int buflen)
{
    if (!buf || buflen < 1) return false;
    strlcpy(buf, s_cur_ip, buflen);
    return s_cur_ip[0] != 0;
}

bool wifi_mgr_get_ssid(char *buf, int buflen)
{
    if (!buf || buflen < 1) return false;
    strlcpy(buf, s_cur_ssid, buflen);
    return s_cur_ssid[0] != 0;
}
