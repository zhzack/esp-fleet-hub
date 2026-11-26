#include "fleet_agent.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "mdns.h"
#include "mbedtls/md.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "cJSON.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "system_caps.h"
#include "wifi_mgr.h"
#if CONFIG_FEATURE_GPIO_CTRL
#include "hal_gpio.h"
#endif
#if CONFIG_FEATURE_CAMERA
#include "hal_camera.h"
#endif
#if CONFIG_FEATURE_PC_MONITOR
#include "mod_pc_monitor.h"
#elif CONFIG_FEATURE_DISPLAY
#include "hal_display.h"
#endif
#if CONFIG_FEATURE_OTA
#include "ota_mgr.h"
#endif
#if CONFIG_FEATURE_WS_GATEWAY
#include "ws_gateway.h"
#endif

static const char *TAG = "fleet_agent";

#if CONFIG_FEATURE_FLEET_AGENT

static volatile bool s_registered = false;
static char s_active_hub_url[128] = "";
static char s_last_pinned_hub[96] = "";
static char s_mdns_hub_url[128] = "";
static int s_pinned_fail_count = 0;
static volatile bool s_force_reload = false;
static TaskHandle_t s_agent_task = NULL;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} http_resp_acc_t;

static esp_err_t http_ev_handler(esp_http_client_event_t *evt)
{
    http_resp_acc_t *acc = (http_resp_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && acc && evt->data_len > 0) {
        size_t avail = (acc->cap > acc->len + 1) ? (acc->cap - acc->len - 1) : 0;
        size_t to_cp = ((size_t)evt->data_len < avail) ? (size_t)evt->data_len : avail;
        if (to_cp > 0) {
            memcpy(acc->buf + acc->len, evt->data, to_cp);
            acc->len += to_cp;
            acc->buf[acc->len] = '\0';
        }
    }
    return ESP_OK;
}

static int http_request_json(const char *url, esp_http_client_method_t method,
                             const char *post_body, char *out_buf, size_t out_cap)
{
    http_resp_acc_t acc = { .buf = out_buf, .cap = out_cap, .len = 0 };
    if (out_buf && out_cap > 0) out_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 5000,
        .event_handler = http_ev_handler,
        .user_data = &acc,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    if (post_body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    return status;
}

static bool query_mdns_hub_url(char *dst, size_t max_len)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_esp-hub", "_tcp", 2000, 2, &results);
    if (err == ESP_OK && results) {
        mdns_result_t *r = results;
        while (r) {
            if (r->addr && r->addr->addr.type == ESP_IPADDR_TYPE_V4) {
                char ip_str[16];
                esp_ip4_addr_t ip4 = r->addr->addr.u_addr.ip4;
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip4));
                snprintf(dst, max_len, "http://%s:%u", ip_str, r->port ? r->port : 8000);
                mdns_query_results_free(results);
                return true;
            }
            r = r->next;
        }
        mdns_query_results_free(results);
    }
    return false;
}

static bool resolve_hub_url(char *dst, size_t max_len)
{
    const app_config_t *cfg = app_config_ptr();
    if (strcmp(s_last_pinned_hub, cfg->hub_url) != 0) {
        strlcpy(s_last_pinned_hub, cfg->hub_url, sizeof(s_last_pinned_hub));
        s_pinned_fail_count = 0;
        s_mdns_hub_url[0] = '\0';
        s_registered = false;
    }

    if (cfg->hub_url[0] != '\0') {
        if (!cfg->hub_fallback_mdns || s_pinned_fail_count < 2) {
            strlcpy(dst, cfg->hub_url, max_len);
            size_t l = strlen(dst);
            while (l > 0 && dst[l - 1] == '/') dst[--l] = '\0';
            return true;
        }
        if (s_mdns_hub_url[0] == '\0') {
            if (query_mdns_hub_url(s_mdns_hub_url, sizeof(s_mdns_hub_url))) {
                ESP_LOGI(TAG, "Pinned Hub unreachable — auto-discovered LAN Hub via mDNS: %s", s_mdns_hub_url);
            }
        }
        if (s_mdns_hub_url[0] != '\0') {
            strlcpy(dst, s_mdns_hub_url, max_len);
            return true;
        }
        // Still try pinned hub if no mDNS hub responded in new LAN
        strlcpy(dst, cfg->hub_url, max_len);
        size_t l = strlen(dst);
        while (l > 0 && dst[l - 1] == '/') dst[--l] = '\0';
        return true;
    }

    return query_mdns_hub_url(dst, max_len);
}

static void compute_hmac_hex(const char *secret, const char *nonce, char out_hex[65])
{
    uint8_t mac[32] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info && mbedtls_md_hmac(info, (const unsigned char *)secret, strlen(secret),
                                (const unsigned char *)nonce, strlen(nonce), mac) == 0) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            out_hex[2 * i]     = hex[mac[i] >> 4];
            out_hex[2 * i + 1] = hex[mac[i] & 0x0F];
        }
        out_hex[64] = '\0';
    } else {
        out_hex[0] = '\0';
    }
}

static bool register_with_hub(const char *hub_base)
{
    const app_config_t *cfg = app_config_ptr();

    char *url  = malloc(192);
    char *resp = malloc(1024);
    if (!url || !resp) {
        free(url);
        free(resp);
        return false;
    }
    char nonce[65] = "";
    char proof[65] = "";

    // 1. Fetch challenge nonce
    snprintf(url, 192, "%s/api/fleet/challenge?device_id=%s", hub_base, cfg->device_id);
    int code = http_request_json(url, HTTP_METHOD_GET, NULL, resp, 1024);
    if (code != 200) {
        ESP_LOGW(TAG, "Hub challenge HTTP %d (%s)", code, hub_base);
        free(url);
        free(resp);
        return false;
    }

    cJSON *ch = cJSON_Parse(resp);
    if (ch) {
        const cJSON *j_n = cJSON_GetObjectItem(ch, "nonce");
        if (cJSON_IsString(j_n)) strlcpy(nonce, j_n->valuestring, sizeof(nonce));
        cJSON_Delete(ch);
    }
    if (cfg->device_secret[0] != '\0' && nonce[0] != '\0') {
        compute_hmac_hex(cfg->device_secret, nonce, proof);
    }

    // 2. Compose comprehensive capabilities + config snapshot payload
    char *caps_raw = malloc(2048);
    char *cfg_raw  = malloc(2048);
    if (!caps_raw || !cfg_raw) {
        free(caps_raw);
        free(cfg_raw);
        free(url);
        free(resp);
        return false;
    }
    system_caps_to_json(caps_raw, 2048);
    app_config_to_json(cfg_raw, 2048);

    cJSON *reg = cJSON_Parse(caps_raw);
    cJSON *cfg_obj = cJSON_Parse(cfg_raw);
    free(caps_raw);
    free(cfg_raw);

    if (!reg) reg = cJSON_CreateObject();
    if (nonce[0]) cJSON_AddStringToObject(reg, "nonce", nonce);
    if (proof[0]) cJSON_AddStringToObject(reg, "proof", proof);
    if (cfg_obj) cJSON_AddItemToObject(reg, "config", cfg_obj);

    system_caps_t *sc = malloc(sizeof(system_caps_t));
    if (sc) {
        system_caps_get(sc);
        cJSON_AddNumberToObject(reg, "total_psram", sc->total_psram);
        free(sc);
    }

    char *body = cJSON_PrintUnformatted(reg);
    cJSON_Delete(reg);
    if (!body) {
        free(url);
        free(resp);
        return false;
    }

    snprintf(url, 192, "%s/api/fleet/register", hub_base);
    code = http_request_json(url, HTTP_METHOD_POST, body, resp, 1024);
    cJSON_free(body);

    bool ok = (code == 200);
    if (ok) {
        ESP_LOGI(TAG, "Registered cleanly with Fleet Hub (%s)!", hub_base);
    } else {
        ESP_LOGW(TAG, "Fleet Hub register rejected HTTP %d: %s", code, resp);
    }
    free(url);
    free(resp);
    return ok;
}

static void reboot_async_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void apply_shadow_sync(cJSON *sync_cfg)
{
    char *cfg_str = cJSON_PrintUnformatted(sync_cfg);
    if (!cfg_str) return;

    app_config_changes_t changes = {0};
    if (app_config_update_from_json_ex(cfg_str, &changes) == ESP_OK) {
        const app_config_t *cfg = app_config_ptr();
        ESP_LOGI(TAG, "Applied shadow config sync from Hub (ver=%lu)", (unsigned long)cfg->config_ver);
        if (changes.mdns_changed) {
            wifi_mgr_update_mdns(cfg->mdns_host, cfg->device_id, cfg->web_port);
        }
#if CONFIG_FEATURE_CAMERA
        if (changes.camera_changed) {
            hal_camera_apply_config(cfg->cam_framesize, cfg->cam_quality, cfg->cam_vflip, cfg->cam_hmirror, cfg->cam_brightness);
        }
#endif
#if CONFIG_FEATURE_PC_MONITOR
        if (changes.display_changed) {
            mod_pc_monitor_apply_config();
        }
#elif CONFIG_FEATURE_DISPLAY
        if (changes.display_changed) {
            hal_display_apply_config();
        }
#endif
        if (changes.auth_changed) {
            wifi_mgr_update_mdns(cfg->mdns_host, cfg->device_id, cfg->web_port);
        }
#if CONFIG_FEATURE_WS_GATEWAY
        if (changes.ws_changed) {
            ws_gateway_apply_config();
        }
#endif
        if (changes.wifi_changed) {
            wifi_mgr_apply_new_sta(cfg->wifi_ssid, cfg->wifi_pass);
        }
        if (changes.hub_changed) {
            fleet_agent_apply_config();
        }
    }
    cJSON_free(cfg_str);
}

static void execute_queued_command(const char *hub_base, const char *dev_id, cJSON *cmd_obj)
{
    const cJSON *j_id = cJSON_GetObjectItem(cmd_obj, "id");
    const cJSON *j_type = cJSON_GetObjectItem(cmd_obj, "type");
    const cJSON *j_pl = cJSON_GetObjectItem(cmd_obj, "payload");
    if (!cJSON_IsNumber(j_id) || !cJSON_IsString(j_type)) return;

    int cmd_id = j_id->valueint;
    const char *ctype = j_type->valuestring;
    bool ok = false;

    ESP_LOGI(TAG, "Executing Hub command #%d (%s)", cmd_id, ctype);

    if (strcmp(ctype, "gpio_set") == 0 && cJSON_IsObject(j_pl)) {
#if CONFIG_FEATURE_GPIO_CTRL
        const cJSON *pin = cJSON_GetObjectItem(j_pl, "pin");
        const cJSON *val = cJSON_GetObjectItem(j_pl, "val");
        const cJSON *duty = cJSON_GetObjectItem(j_pl, "duty");
        if (cJSON_IsNumber(pin)) {
            if (cJSON_IsNumber(duty)) ok = (hal_gpio_set_pwm(pin->valueint, (uint8_t)duty->valueint) == ESP_OK);
            else if (cJSON_IsNumber(val)) ok = (hal_gpio_write(pin->valueint, val->valueint) == ESP_OK);
        }
#endif
    } else if (strcmp(ctype, "ota_pull") == 0 && cJSON_IsObject(j_pl)) {
#if CONFIG_FEATURE_OTA
        const cJSON *j_url = cJSON_GetObjectItem(j_pl, "url");
        const cJSON *j_sha = cJSON_GetObjectItem(j_pl, "sha256");
        if (cJSON_IsString(j_url)) {
            esp_err_t ota_err = ota_mgr_pull_from_url(
                j_url->valuestring,
                cJSON_IsString(j_sha) ? j_sha->valuestring : "",
                false
            );
            ok = (ota_err == ESP_OK);
        }
#endif
    } else if (strcmp(ctype, "reboot") == 0) {
        ok = true;
    }

    // Acknowledge completion to Hub before reboot/OTA
    char ack_url[192];
    char ack_body[128];
    char dummy[128];
    snprintf(ack_url, sizeof(ack_url), "%s/api/fleet/cmd_ack", hub_base);
    snprintf(ack_body, sizeof(ack_body), "{\"cmd_id\":%d,\"device_id\":\"%s\",\"ok\":%s}",
             cmd_id, dev_id, ok ? "true" : "false");
    http_request_json(ack_url, HTTP_METHOD_POST, ack_body, dummy, sizeof(dummy));

    if ((strcmp(ctype, "reboot") == 0) || (strcmp(ctype, "ota_pull") == 0 && ok)) {
        xTaskCreate(reboot_async_task, "hub_reboot", 2048, NULL, 5, NULL);
    }
}

static void fleet_agent_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        if (s_force_reload) {
            s_force_reload = false;
            s_registered = false;
            s_pinned_fail_count = 0;
            s_mdns_hub_url[0] = '\0';
        }

        if (!wifi_mgr_connected()) {
            s_registered = false;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
            continue;
        }

        if (!resolve_hub_url(s_active_hub_url, sizeof(s_active_hub_url))) {
            s_registered = false;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(6000));
            continue;
        }

        const app_config_t *cfg = app_config_ptr();
        uint32_t hb_ms = cfg->hb_interval_ms;
        if (hb_ms < 2000) hb_ms = 2000;
        if (hb_ms > 120000) hb_ms = 120000;

        if (!s_registered) {
            if (register_with_hub(s_active_hub_url)) {
                s_registered = true;
                s_pinned_fail_count = 0;
#if CONFIG_FEATURE_WS_GATEWAY
                ws_gateway_apply_config();
#endif
            } else {
                if (cfg->hub_url[0] && strncmp(s_active_hub_url, cfg->hub_url, strlen(s_active_hub_url)) == 0) {
                    s_pinned_fail_count++;
                } else {
                    s_mdns_hub_url[0] = '\0';
                }
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hb_ms < 6000 ? hb_ms : 6000));
                continue;
            }
        }

        // Send periodic Heartbeat + poll shadow/commands
        cfg = app_config_ptr();
        system_caps_t *sc = malloc(sizeof(system_caps_t));
        if (!sc) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
        system_caps_get(sc);

        cJSON *hb = cJSON_CreateObject();
        cJSON_AddStringToObject(hb, "device_id", cfg->device_id);
        cJSON_AddNumberToObject(hb, "rssi", sc->wifi_rssi);
        cJSON_AddNumberToObject(hb, "free_heap", sc->free_heap);
        cJSON_AddNumberToObject(hb, "uptime_sec", sc->uptime_sec);
        cJSON_AddStringToObject(hb, "ip", sc->ip_addr);
        cJSON_AddNumberToObject(hb, "config_ver", cfg->config_ver);
        free(sc);
        char *hb_str = cJSON_PrintUnformatted(hb);
        cJSON_Delete(hb);

        if (hb_str) {
            char *url  = malloc(192);
            char *resp = malloc(1024);
            int code = -1;
            if (url && resp) {
                snprintf(url, 192, "%s/api/fleet/heartbeat", s_active_hub_url);
                code = http_request_json(url, HTTP_METHOD_POST, hb_str, resp, 1024);
            }
            cJSON_free(hb_str);

            if (code == 200 && resp) {
                s_pinned_fail_count = 0;
                cJSON *r = cJSON_Parse(resp);
                if (r) {
                    const cJSON *rereg = cJSON_GetObjectItem(r, "reregister");
                    if (cJSON_IsBool(rereg) && cJSON_IsTrue(rereg)) {
                        s_registered = false;
                    }
                    cJSON *sync_cfg = cJSON_GetObjectItem(r, "sync_config");
                    if (cJSON_IsObject(sync_cfg)) {
                        apply_shadow_sync(sync_cfg);
                    }
                    cJSON *cmds = cJSON_GetObjectItem(r, "commands");
                    if (cJSON_IsArray(cmds)) {
                        cJSON *c = NULL;
                        cJSON_ArrayForEach(c, cmds) {
                            execute_queued_command(s_active_hub_url, cfg->device_id, c);
                        }
                    }
                    cJSON_Delete(r);
                }
            } else {
                ESP_LOGW(TAG, "Heartbeat error HTTP %d", code);
                if (code < 0) {
                    s_registered = false;
                    if (cfg->hub_url[0] && strncmp(s_active_hub_url, cfg->hub_url, strlen(s_active_hub_url)) == 0) {
                        s_pinned_fail_count++;
                    } else {
                        s_mdns_hub_url[0] = '\0';
                    }
                }
            }
            free(url);
            free(resp);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hb_ms));
    }
}

esp_err_t fleet_agent_init(void)
{
    BaseType_t ok = xTaskCreate(fleet_agent_task, "fleet_agent", 9216, NULL, 3, &s_agent_task);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

bool fleet_agent_is_registered(void)
{
    return s_registered;
}

void fleet_agent_get_hub_url(char *dst, size_t max_len)
{
    if (!dst || max_len == 0) return;
    strlcpy(dst, s_active_hub_url, max_len);
}

void fleet_agent_apply_config(void)
{
    s_force_reload = true;
    if (s_agent_task) {
        xTaskNotifyGive(s_agent_task);
    }
}

#else
esp_err_t fleet_agent_init(void) { return ESP_OK; }
bool fleet_agent_is_registered(void) { return false; }
void fleet_agent_get_hub_url(char *dst, size_t max_len) { if (dst && max_len) dst[0] = '\0'; }
void fleet_agent_apply_config(void) {}
#endif
