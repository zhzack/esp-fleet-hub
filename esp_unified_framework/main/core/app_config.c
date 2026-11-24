#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cfg";
static app_config_t s_cfg;

#ifdef CONFIG_APP_DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID  CONFIG_APP_DEFAULT_WIFI_SSID
#else
#define DEFAULT_WIFI_SSID  "1"
#endif

#ifdef CONFIG_APP_DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS  CONFIG_APP_DEFAULT_WIFI_PASS
#else
#define DEFAULT_WIFI_PASS  "11111111"
#endif

#ifdef CONFIG_APP_DEFAULT_DEVICE_ID
#define DEFAULT_DEVICE_ID  CONFIG_APP_DEFAULT_DEVICE_ID
#else
#define DEFAULT_DEVICE_ID  "esp_unified_node"
#endif

#ifdef CONFIG_APP_MDNS_HOST
#define DEFAULT_MDNS_HOST  CONFIG_APP_MDNS_HOST
#else
#define DEFAULT_MDNS_HOST  "esp-node"
#endif

static void apply_defaults(app_config_t *cfg)
{
    strlcpy(cfg->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, DEFAULT_WIFI_PASS, sizeof(cfg->wifi_pass));
    strlcpy(cfg->device_id, DEFAULT_DEVICE_ID, sizeof(cfg->device_id));
    strlcpy(cfg->mdns_host, DEFAULT_MDNS_HOST, sizeof(cfg->mdns_host));
    cfg->web_port = 80;
}

static void nvs_read_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        // preserve existing default
    }
}

esp_err_t app_config_init(void)
{
    apply_defaults(&s_cfg);
    return app_config_load(&s_cfg);
}

esp_err_t app_config_load(app_config_t *cfg)
{
    apply_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open("app", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No NVS config partition, using firmware defaults (ssid='%s')", cfg->wifi_ssid);
        memcpy(&s_cfg, cfg, sizeof(s_cfg));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    nvs_read_str(h, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    nvs_read_str(h, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    nvs_read_str(h, "device_id", cfg->device_id, sizeof(cfg->device_id));
    nvs_read_str(h, "mdns_host", cfg->mdns_host, sizeof(cfg->mdns_host));
    uint16_t port = 80;
    if (nvs_get_u16(h, "web_port", &port) == ESP_OK) {
        cfg->web_port = port;
    }

    nvs_close(h);

    if (cfg->wifi_ssid[0] == '\0') {
        strlcpy(cfg->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_pass, DEFAULT_WIFI_PASS, sizeof(cfg->wifi_pass));
    }

    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    ESP_LOGI(TAG, "Config loaded: device_id='%s' ssid='%s' mdns='%s'",
             cfg->device_id, cfg->wifi_ssid, cfg->mdns_host);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("app", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "wifi_ssid", cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass", cfg->wifi_pass);
    nvs_set_str(h, "device_id", cfg->device_id);
    nvs_set_str(h, "mdns_host", cfg->mdns_host);
    nvs_set_u16(h, "web_port", cfg->web_port);

    err = nvs_commit(h);
    nvs_close(h);

    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    ESP_LOGI(TAG, "Config saved to NVS!");
    return err;
}

void app_config_get(app_config_t *out_cfg)
{
    if (out_cfg) {
        memcpy(out_cfg, &s_cfg, sizeof(s_cfg));
    }
}
