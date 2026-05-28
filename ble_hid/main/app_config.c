#include "app_config.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";
static app_config_t s_cfg;

/* Compile-time defaults — override via NVS in production. */
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID  "MyAP"
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS  "changeme"
#endif
#ifndef DEFAULT_SERVER_HOST
#define DEFAULT_SERVER_HOST "192.168.1.10"
#endif
#ifndef DEFAULT_SERVER_PORT
#define DEFAULT_SERVER_PORT 8000
#endif
#ifndef DEFAULT_DEVICE_ID
#define DEFAULT_DEVICE_ID  "esp32_001"
#endif

static void apply_defaults(app_config_t *cfg)
{
    cfg->wifi_ssid[0] = '\0';
    cfg->wifi_pass[0] = '\0';
    strlcpy(cfg->server_host,  DEFAULT_SERVER_HOST,  sizeof cfg->server_host);
    cfg->server_port = DEFAULT_SERVER_PORT;
    strlcpy(cfg->device_id,    DEFAULT_DEVICE_ID,    sizeof cfg->device_id);
    cfg->device_token[0] = '\0';
    cfg->strict_seq   = false;
    cfg->queue_size   = 128;
    cfg->queue_policy = 0;
}

static void nvs_read_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        /* leave default */
    }
}

esp_err_t app_config_load(app_config_t *cfg)
{
    apply_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open("app", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "no \"app\" namespace yet — using defaults");
        cfg->wifi_ssid[0] = '\0';
        cfg->wifi_pass[0] = '\0';
        memcpy(&s_cfg, cfg, sizeof s_cfg);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    nvs_read_str(h, "wifi_ssid",    cfg->wifi_ssid,    sizeof cfg->wifi_ssid);
    nvs_read_str(h, "wifi_pass",    cfg->wifi_pass,    sizeof cfg->wifi_pass);
    nvs_read_str(h, "server_host",  cfg->server_host,  sizeof cfg->server_host);
    uint16_t port = cfg->server_port;
    nvs_get_u16(h, "server_port", &port);
    cfg->server_port = port;
    nvs_read_str(h, "device_id",    cfg->device_id,    sizeof cfg->device_id);
    nvs_read_str(h, "device_token", cfg->device_token, sizeof cfg->device_token);
    uint8_t b = cfg->strict_seq ? 1 : 0;
    nvs_get_u8(h, "strict_seq", &b);
    cfg->strict_seq = b != 0;
    nvs_get_u16(h, "queue_size", &cfg->queue_size);
    nvs_get_u8(h, "queue_policy", &cfg->queue_policy);

    nvs_close(h);
    memcpy(&s_cfg, cfg, sizeof s_cfg);

    ESP_LOGI(TAG, "loaded: ssid=%s host=%s:%u device_id=%s queue=%u policy=%u strict_seq=%d",
             cfg->wifi_ssid, cfg->server_host, cfg->server_port,
             cfg->device_id, cfg->queue_size, cfg->queue_policy, cfg->strict_seq);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("app", NVS_READWRITE, &h));
    nvs_set_str(h, "wifi_ssid",    cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass",    cfg->wifi_pass);
    nvs_set_str(h, "server_host",  cfg->server_host);
    nvs_set_u16(h, "server_port",  cfg->server_port);
    nvs_set_str(h, "device_id",    cfg->device_id);
    nvs_set_str(h, "device_token", cfg->device_token);
    nvs_set_u8 (h, "strict_seq",   cfg->strict_seq ? 1 : 0);
    nvs_set_u16(h, "queue_size",   cfg->queue_size);
    nvs_set_u8 (h, "queue_policy", cfg->queue_policy);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    memcpy(&s_cfg, cfg, sizeof s_cfg);
    return err;
}

const app_config_t *app_config_get(void) { return &s_cfg; }
