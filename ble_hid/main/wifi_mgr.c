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
static int  s_max_retry = 5;
static bool s_connected = false;
static bool s_inited = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static char s_cur_ssid[33] = {0};
static char s_cur_ip[16]   = {0};

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_cur_ip[0] = 0;
        if (s_retry < s_max_retry) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "retry %d/%d", s_retry, s_max_retry);
        } else {
            xEventGroupSetBits(s_eg, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_cur_ip, sizeof s_cur_ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got ip: %s", s_cur_ip);
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
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    strlcpy(s_cur_ssid, ssid, sizeof s_cur_ssid);
    esp_wifi_disconnect();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_CONNECTED | BIT_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
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
    wifi_scan_config_t sc = { 0 };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) { *out_n = 0; return err; }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > WIFI_SCAN_MAX) n = WIFI_SCAN_MAX;
    wifi_ap_record_t recs[WIFI_SCAN_MAX];
    esp_wifi_scan_get_ap_records(&n, recs);

    for (int i = 0; i < n; ++i) {
        strlcpy(out_arr[i].ssid, (const char *)recs[i].ssid, sizeof out_arr[i].ssid);
        out_arr[i].rssi = recs[i].rssi;
        out_arr[i].auth = recs[i].authmode;
    }
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
