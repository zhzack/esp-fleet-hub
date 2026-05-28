#include "provision.h"
#include "provision_html.h"
#include "wifi_mgr.h"
#include "app_config.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "cJSON.h"

#define TAG "PROVISION"
#define BIT_DONE BIT0

static httpd_handle_t s_httpd = NULL;
static EventGroupHandle_t s_evt = NULL;

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PROVISION_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_entry_t arr[20];
    int n = 20;
    esp_err_t err = wifi_mgr_scan(arr, &n);
    cJSON *root = cJSON_CreateObject();
    cJSON *nets = cJSON_CreateArray();
    if (err == ESP_OK) {
        for (int i = 0; i < n; i++) {
            if (!arr[i].ssid[0]) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", arr[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", arr[i].rssi);
            cJSON_AddNumberToObject(o, "auth", arr[i].auth);
            cJSON_AddItemToArray(nets, o);
        }
    }
    cJSON_AddItemToObject(root, "networks", nets);
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, int max)
{
    int total = req->content_len;
    if (total <= 0 || total >= max) return ESP_FAIL;
    int r = 0;
    while (r < total) {
        int x = httpd_req_recv(req, buf + r, total - r);
        if (x <= 0) return ESP_FAIL;
        r += x;
    }
    buf[r] = 0;
    return ESP_OK;
}

static void send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
}

static esp_err_t h_connect(httpd_req_t *req)
{
    char buf[512];
    if (read_body(req, buf, sizeof(buf)) != ESP_OK) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "bad body");
        send_json(req, r);
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "bad json");
        send_json(req, r);
        return ESP_OK;
    }
    const cJSON *jssid = cJSON_GetObjectItem(j, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(j, "pass");
    const cJSON *jhost = cJSON_GetObjectItem(j, "host");
    const cJSON *jport = cJSON_GetObjectItem(j, "port");
    const char *ssid = (jssid && cJSON_IsString(jssid)) ? jssid->valuestring : NULL;
    const char *pass = (jpass && cJSON_IsString(jpass)) ? jpass->valuestring : "";
    const char *host = (jhost && cJSON_IsString(jhost)) ? jhost->valuestring : NULL;
    int port = (jport && cJSON_IsNumber(jport)) ? jport->valueint : 0;
    if (!ssid || !ssid[0]) {
        cJSON_Delete(j);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "missing ssid");
        send_json(req, r);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "trying ssid=%s host=%s port=%d", ssid, host ? host : "", port);
    esp_err_t err = wifi_mgr_sta_connect(ssid, pass, 15000);
    if (err != ESP_OK) {
        cJSON_Delete(j);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "connect failed");
        send_json(req, r);
        return ESP_OK;
    }

    app_config_t cfg;
    app_config_load(&cfg);
    strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
    if (host && host[0]) strlcpy(cfg.server_host, host, sizeof(cfg.server_host));
    if (port > 0 && port < 65536) cfg.server_port = (uint16_t)port;
    esp_err_t serr = app_config_save(&cfg);
    if (serr != ESP_OK) ESP_LOGW(TAG, "cfg save: %s", esp_err_to_name(serr));

    char ip[16] = {0};
    wifi_mgr_get_ip(ip, sizeof(ip));
    cJSON_Delete(j);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "ip", ip);
    send_json(req, r);

    xEventGroupSetBits(s_evt, BIT_DONE);
    return ESP_OK;
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    char ssid[33] = {0};
    char ip[16] = {0};
    bool conn = wifi_mgr_connected();
    wifi_mgr_get_ssid(ssid, sizeof(ssid));
    wifi_mgr_get_ip(ip, sizeof(ip));
    cJSON_AddBoolToObject(r, "connected", conn);
    cJSON_AddStringToObject(r, "ssid", ssid);
    cJSON_AddStringToObject(r, "ip", ip);
    send_json(req, r);
    return ESP_OK;
}

static void register_routes(httpd_handle_t s)
{
    httpd_uri_t u_root    = { .uri="/",            .method=HTTP_GET,  .handler=h_root };
    httpd_uri_t u_scan    = { .uri="/api/scan",    .method=HTTP_GET,  .handler=h_scan };
    httpd_uri_t u_connect = { .uri="/api/connect", .method=HTTP_POST, .handler=h_connect };
    httpd_uri_t u_status  = { .uri="/api/status",  .method=HTTP_GET,  .handler=h_status };
    httpd_register_uri_handler(s, &u_root);
    httpd_register_uri_handler(s, &u_scan);
    httpd_register_uri_handler(s, &u_connect);
    httpd_register_uri_handler(s, &u_status);
}

esp_err_t provision_run(int timeout_ms)
{
    ESP_LOGI(TAG, "provision start");
    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;

    wifi_mgr_init();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[33];
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP32-HID-%02X%02X", mac[4], mac[5]);
    if (wifi_mgr_ap_start(ap_ssid) != ESP_OK) {
        ESP_LOGE(TAG, "ap start failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SoftAP up, ssid=%s, http://192.168.4.1/", ap_ssid);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = 80;
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        wifi_mgr_ap_stop();
        return ESP_FAIL;
    }
    register_routes(s_httpd);

    TickType_t wait = (timeout_ms > 0) ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_DONE, pdFALSE, pdFALSE, wait);

    vTaskDelay(pdMS_TO_TICKS(500));
    httpd_stop(s_httpd);
    s_httpd = NULL;
    wifi_mgr_ap_stop();
    vEventGroupDelete(s_evt);
    s_evt = NULL;
    ESP_LOGI(TAG, "provision done bits=%lu", (unsigned long)bits);
    return (bits & BIT_DONE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
