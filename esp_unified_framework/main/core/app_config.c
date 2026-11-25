#include "app_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
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
#define DEFAULT_DEVICE_ID  ""
#endif

#ifdef CONFIG_APP_MDNS_HOST
#define DEFAULT_MDNS_HOST  CONFIG_APP_MDNS_HOST
#else
#define DEFAULT_MDNS_HOST  "esp-node"
#endif

#ifdef CONFIG_APP_DEFAULT_HUB_URL
#define DEFAULT_HUB_URL    CONFIG_APP_DEFAULT_HUB_URL
#else
#define DEFAULT_HUB_URL    ""
#endif

#ifdef CONFIG_APP_DEFAULT_WS_GATEWAY_URL
#define DEFAULT_WS_GATEWAY_URL CONFIG_APP_DEFAULT_WS_GATEWAY_URL
#else
#define DEFAULT_WS_GATEWAY_URL ""
#endif

static void make_mac_device_id(char *dst, size_t max_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#if CONFIG_IDF_TARGET_ESP32C3
    const char *prefix = "esp32c3";
#elif CONFIG_IDF_TARGET_ESP32S3
    const char *prefix = "esp32s3";
#elif CONFIG_IDF_TARGET_ESP32C2
    const char *prefix = "esp32c2";
#else
    const char *prefix = "esp32";
#endif
    snprintf(dst, max_len, "%s-%02x%02x%02x", prefix, mac[3], mac[4], mac[5]);
}

static void apply_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, DEFAULT_WIFI_PASS, sizeof(cfg->wifi_pass));
    if (DEFAULT_DEVICE_ID[0] != '\0') {
        strlcpy(cfg->device_id, DEFAULT_DEVICE_ID, sizeof(cfg->device_id));
    } else {
        make_mac_device_id(cfg->device_id, sizeof(cfg->device_id));
    }
    strlcpy(cfg->mdns_host, DEFAULT_MDNS_HOST, sizeof(cfg->mdns_host));
    strlcpy(cfg->hub_url, DEFAULT_HUB_URL, sizeof(cfg->hub_url));
    strlcpy(cfg->ws_gateway_url, DEFAULT_WS_GATEWAY_URL, sizeof(cfg->ws_gateway_url));
    cfg->web_port = 80;
    cfg->config_ver = 1;

    cfg->cam_framesize = 8; // FRAMESIZE_VGA
    cfg->cam_quality = 12;
    cfg->cam_vflip = true;
    cfg->cam_hmirror = false;
    cfg->cam_brightness = 1;
    strlcpy(cfg->gpio_cfg_json, "[]", sizeof(cfg->gpio_cfg_json));
}

static void nvs_read_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    nvs_get_str(h, key, dst, &len);
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
        ESP_LOGW(TAG, "No NVS config yet, using defaults (id='%s', ssid='%s')",
                 cfg->device_id, cfg->wifi_ssid);
        memcpy(&s_cfg, cfg, sizeof(s_cfg));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    nvs_read_str(h, "wifi_ssid",   cfg->wifi_ssid,     sizeof(cfg->wifi_ssid));
    nvs_read_str(h, "wifi_pass",   cfg->wifi_pass,     sizeof(cfg->wifi_pass));
    nvs_read_str(h, "wifi_ssid_b", cfg->wifi_ssid_bak, sizeof(cfg->wifi_ssid_bak));
    nvs_read_str(h, "wifi_pass_b", cfg->wifi_pass_bak, sizeof(cfg->wifi_pass_bak));
    nvs_read_str(h, "device_id",   cfg->device_id,     sizeof(cfg->device_id));
    nvs_read_str(h, "mdns_host",   cfg->mdns_host,     sizeof(cfg->mdns_host));
    nvs_read_str(h, "hub_url",     cfg->hub_url,        sizeof(cfg->hub_url));
    nvs_read_str(h, "gw_ws_url",   cfg->ws_gateway_url, sizeof(cfg->ws_gateway_url));
    nvs_read_str(h, "dev_secret",  cfg->device_secret,  sizeof(cfg->device_secret));
    nvs_read_str(h, "api_token",   cfg->api_token,     sizeof(cfg->api_token));
    nvs_read_str(h, "gpio_cfg",    cfg->gpio_cfg_json, sizeof(cfg->gpio_cfg_json));

    uint16_t port = 80;
    if (nvs_get_u16(h, "web_port", &port) == ESP_OK && port > 0) {
        cfg->web_port = port;
    }
    uint32_t ver = 1;
    if (nvs_get_u32(h, "cfg_ver", &ver) == ESP_OK && ver > 0) {
        cfg->config_ver = ver;
    }

    uint8_t u8 = 0;
    int8_t i8 = 0;
    if (nvs_get_u8(h, "cam_fs", &u8) == ESP_OK) cfg->cam_framesize = u8;
    if (nvs_get_u8(h, "cam_q", &u8) == ESP_OK && u8 >= 4 && u8 <= 63) cfg->cam_quality = u8;
    if (nvs_get_u8(h, "cam_vflip", &u8) == ESP_OK) cfg->cam_vflip = (u8 != 0);
    if (nvs_get_u8(h, "cam_hmir", &u8) == ESP_OK) cfg->cam_hmirror = (u8 != 0);
    if (nvs_get_i8(h, "cam_bri", &i8) == ESP_OK) cfg->cam_brightness = i8;

    nvs_close(h);

    if (cfg->wifi_ssid[0] == '\0') {
        strlcpy(cfg->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_pass, DEFAULT_WIFI_PASS, sizeof(cfg->wifi_pass));
    }
    if (cfg->device_id[0] == '\0') {
        make_mac_device_id(cfg->device_id, sizeof(cfg->device_id));
    }

    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    ESP_LOGI(TAG, "Config loaded: id='%s' ssid='%s' ver=%lu auth=%s",
             cfg->device_id, cfg->wifi_ssid, (unsigned long)cfg->config_ver,
             cfg->api_token[0] ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open("app", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "wifi_ssid",   cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass",   cfg->wifi_pass);
    nvs_set_str(h, "wifi_ssid_b", cfg->wifi_ssid_bak);
    nvs_set_str(h, "wifi_pass_b", cfg->wifi_pass_bak);
    nvs_set_str(h, "device_id",   cfg->device_id);
    nvs_set_str(h, "mdns_host",   cfg->mdns_host);
    nvs_set_str(h, "hub_url",     cfg->hub_url);
    nvs_set_str(h, "gw_ws_url",   cfg->ws_gateway_url);
    nvs_set_str(h, "dev_secret",  cfg->device_secret);
    nvs_set_str(h, "api_token",   cfg->api_token);
    nvs_set_str(h, "gpio_cfg",    cfg->gpio_cfg_json);
    nvs_set_u16(h, "web_port",    cfg->web_port);
    nvs_set_u32(h, "cfg_ver",     cfg->config_ver);
    nvs_set_u8(h,  "cam_fs",      cfg->cam_framesize);
    nvs_set_u8(h,  "cam_q",       cfg->cam_quality);
    nvs_set_u8(h,  "cam_vflip",   cfg->cam_vflip ? 1 : 0);
    nvs_set_u8(h,  "cam_hmir",    cfg->cam_hmirror ? 1 : 0);
    nvs_set_i8(h,  "cam_bri",     cfg->cam_brightness);

    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        memcpy(&s_cfg, cfg, sizeof(s_cfg));
        ESP_LOGI(TAG, "Config committed to NVS Flash (ver=%lu)", (unsigned long)cfg->config_ver);
    }
    return err;
}

void app_config_get(app_config_t *out_cfg)
{
    if (out_cfg) {
        memcpy(out_cfg, &s_cfg, sizeof(s_cfg));
    }
}

const app_config_t *app_config_ptr(void)
{
    return &s_cfg;
}

size_t app_config_to_json(char *dst, size_t max_len)
{
    if (!dst || max_len < 32) return 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "wifi_ssid", s_cfg.wifi_ssid);
    cJSON_AddBoolToObject(root, "has_wifi_pass", s_cfg.wifi_pass[0] != '\0');
    cJSON_AddStringToObject(root, "device_id", s_cfg.device_id);
    cJSON_AddStringToObject(root, "mdns_host", s_cfg.mdns_host);
    cJSON_AddNumberToObject(root, "web_port", s_cfg.web_port);
    cJSON_AddStringToObject(root, "hub_url", s_cfg.hub_url);
    cJSON_AddStringToObject(root, "ws_gateway_url", s_cfg.ws_gateway_url);
    cJSON_AddBoolToObject(root, "has_device_secret", s_cfg.device_secret[0] != '\0');
    cJSON_AddBoolToObject(root, "auth_required", s_cfg.api_token[0] != '\0');
    cJSON_AddNumberToObject(root, "config_ver", s_cfg.config_ver);

    cJSON *cam = cJSON_AddObjectToObject(root, "camera");
    cJSON_AddNumberToObject(cam, "framesize", s_cfg.cam_framesize);
    cJSON_AddNumberToObject(cam, "quality", s_cfg.cam_quality);
    cJSON_AddBoolToObject(cam, "vflip", s_cfg.cam_vflip);
    cJSON_AddBoolToObject(cam, "hmirror", s_cfg.cam_hmirror);
    cJSON_AddNumberToObject(cam, "brightness", s_cfg.cam_brightness);

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

esp_err_t app_config_update_from_json_ex(const char *json_str, app_config_changes_t *out_changes)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    if (out_changes) memset(out_changes, 0, sizeof(*out_changes));

    cJSON *root = cJSON_Parse(json_str);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t updated;
    memcpy(&updated, &s_cfg, sizeof(updated));
    bool dirty = false;

    const cJSON *j_ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(root, "wifi_pass");
    if (cJSON_IsString(j_ssid) && j_ssid->valuestring[0] != '\0') {
        if (strcmp(updated.wifi_ssid, j_ssid->valuestring) != 0) {
            // Backup working WiFi credentials before changing
            strlcpy(updated.wifi_ssid_bak, s_cfg.wifi_ssid, sizeof(updated.wifi_ssid_bak));
            strlcpy(updated.wifi_pass_bak, s_cfg.wifi_pass, sizeof(updated.wifi_pass_bak));
            strlcpy(updated.wifi_ssid, j_ssid->valuestring, sizeof(updated.wifi_ssid));
            if (out_changes) out_changes->wifi_changed = true;
            dirty = true;
        }
    }
    if (cJSON_IsString(j_pass) && strcmp(updated.wifi_pass, j_pass->valuestring) != 0) {
        strlcpy(updated.wifi_pass_bak, s_cfg.wifi_pass, sizeof(updated.wifi_pass_bak));
        if (updated.wifi_ssid_bak[0] == '\0') {
            strlcpy(updated.wifi_ssid_bak, s_cfg.wifi_ssid, sizeof(updated.wifi_ssid_bak));
        }
        strlcpy(updated.wifi_pass, j_pass->valuestring, sizeof(updated.wifi_pass));
        if (out_changes) out_changes->wifi_changed = true;
        dirty = true;
    }

    const cJSON *j_dev = cJSON_GetObjectItem(root, "device_id");
    if (cJSON_IsString(j_dev) && j_dev->valuestring[0] != '\0' && strcmp(updated.device_id, j_dev->valuestring) != 0) {
        strlcpy(updated.device_id, j_dev->valuestring, sizeof(updated.device_id));
        if (out_changes) out_changes->mdns_changed = true;
        dirty = true;
    }

    const cJSON *j_mdns = cJSON_GetObjectItem(root, "mdns_host");
    if (cJSON_IsString(j_mdns) && j_mdns->valuestring[0] != '\0' && strcmp(updated.mdns_host, j_mdns->valuestring) != 0) {
        strlcpy(updated.mdns_host, j_mdns->valuestring, sizeof(updated.mdns_host));
        if (out_changes) out_changes->mdns_changed = true;
        dirty = true;
    }

    const cJSON *j_port = cJSON_GetObjectItem(root, "web_port");
    if (cJSON_IsNumber(j_port)) {
        int port = j_port->valueint;
        if (port > 0 && port < 65536 && updated.web_port != (uint16_t)port) {
            updated.web_port = (uint16_t)port;
            if (out_changes) out_changes->port_changed = true;
            dirty = true;
        }
    }

    const cJSON *j_hub = cJSON_GetObjectItem(root, "hub_url");
    if (cJSON_IsString(j_hub) && strcmp(updated.hub_url, j_hub->valuestring) != 0) {
        strlcpy(updated.hub_url, j_hub->valuestring, sizeof(updated.hub_url));
        dirty = true;
    }

    const cJSON *j_gw = cJSON_GetObjectItem(root, "ws_gateway_url");
    if (cJSON_IsString(j_gw) && strcmp(updated.ws_gateway_url, j_gw->valuestring) != 0) {
        strlcpy(updated.ws_gateway_url, j_gw->valuestring, sizeof(updated.ws_gateway_url));
        dirty = true;
    }

    const cJSON *j_sec = cJSON_GetObjectItem(root, "device_secret");
    if (cJSON_IsString(j_sec) && strcmp(updated.device_secret, j_sec->valuestring) != 0) {
        strlcpy(updated.device_secret, j_sec->valuestring, sizeof(updated.device_secret));
        dirty = true;
    }

    const cJSON *j_tok = cJSON_GetObjectItem(root, "api_token");
    if (cJSON_IsString(j_tok) && strcmp(updated.api_token, j_tok->valuestring) != 0) {
        strlcpy(updated.api_token, j_tok->valuestring, sizeof(updated.api_token));
        if (out_changes) out_changes->auth_changed = true;
        dirty = true;
    }

    const cJSON *j_ver = cJSON_GetObjectItem(root, "config_ver");
    const cJSON *j_cam = cJSON_GetObjectItem(root, "camera");
    if (cJSON_IsObject(j_cam)) {
        const cJSON *fs = cJSON_GetObjectItem(j_cam, "framesize");
        const cJSON *q  = cJSON_GetObjectItem(j_cam, "quality");
        const cJSON *vf = cJSON_GetObjectItem(j_cam, "vflip");
        const cJSON *hm = cJSON_GetObjectItem(j_cam, "hmirror");
        const cJSON *br = cJSON_GetObjectItem(j_cam, "brightness");
        if (cJSON_IsNumber(fs)) updated.cam_framesize = (uint8_t)fs->valueint;
        if (cJSON_IsNumber(q) && q->valueint >= 4 && q->valueint <= 63) updated.cam_quality = (uint8_t)q->valueint;
        if (cJSON_IsBool(vf)) updated.cam_vflip = cJSON_IsTrue(vf);
        if (cJSON_IsBool(hm)) updated.cam_hmirror = cJSON_IsTrue(hm);
        if (cJSON_IsNumber(br)) updated.cam_brightness = (int8_t)br->valueint;
        if (out_changes) out_changes->camera_changed = true;
        dirty = true;
    }

    if (cJSON_IsNumber(j_ver) && (uint32_t)j_ver->valuedouble > updated.config_ver) {
        updated.config_ver = (uint32_t)j_ver->valuedouble;
    } else if (dirty) {
        updated.config_ver++;
    }

    cJSON_Delete(root);
    return dirty ? app_config_save(&updated) : ESP_OK;
}

esp_err_t app_config_update_from_json(const char *json_str)
{
    return app_config_update_from_json_ex(json_str, NULL);
}

esp_err_t app_config_backup_wifi(const char *ssid, const char *pass)
{
    static app_config_t updated;
    memcpy(&updated, &s_cfg, sizeof(updated));
    if (ssid) strlcpy(updated.wifi_ssid_bak, ssid, sizeof(updated.wifi_ssid_bak));
    if (pass) strlcpy(updated.wifi_pass_bak, pass, sizeof(updated.wifi_pass_bak));
    return app_config_save(&updated);
}

esp_err_t app_config_rollback_wifi(void)
{
    if (s_cfg.wifi_ssid_bak[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    static app_config_t updated;
    memcpy(&updated, &s_cfg, sizeof(updated));
    strlcpy(updated.wifi_ssid, s_cfg.wifi_ssid_bak, sizeof(updated.wifi_ssid));
    strlcpy(updated.wifi_pass, s_cfg.wifi_pass_bak, sizeof(updated.wifi_pass));
    updated.config_ver++;
    ESP_LOGW(TAG, "Rolling back WiFi credentials to backup SSID: '%s'", updated.wifi_ssid);
    return app_config_save(&updated);
}

esp_err_t app_config_save_gpio_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    app_config_t updated;
    memcpy(&updated, &s_cfg, sizeof(updated));
    strlcpy(updated.gpio_cfg_json, json_str, sizeof(updated.gpio_cfg_json));
    updated.config_ver++;
    return app_config_save(&updated);
}

esp_err_t app_config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("app", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        err = nvs_commit(h);
        nvs_close(h);
    }
    apply_defaults(&s_cfg);
    ESP_LOGW(TAG, "Factory reset completed on NVS 'app' namespace!");
    return err;
}
