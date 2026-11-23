#include "lan_mode.h"
#include "cmd_router.h"
#include "wifi_mgr.h"
#include "log_capture.h"
#include "macro_mgr.h"
#include "ws_hid.h"
#include "app_config.h"
#include "hid_usb.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mdns.h"

#define TAG "LAN"
#define DISCOVERY_PORT 42424
#define HTTP_PORT 80
#define DISCOVERY_MAGIC "ESP32_HID_DISCOVER"

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t   s_udp_task = NULL;
static volatile bool  s_udp_run = false;
static char           s_device_id[33] = {0};

/* ---------------- CORS & Auth Helpers ---------------- */

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type, Sec-WebSocket-Protocol, Sec-WebSocket-Key");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

static esp_err_t send_json_resp(httpd_req_t *req, int code, const char *err, cJSON *result_owned)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", code == 0);
    cJSON_AddNumberToObject(root, "code", code);
    if (err && err[0]) cJSON_AddStringToObject(root, "error", err);
    if (result_owned) cJSON_AddItemToObject(root, "result", result_owned);
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_set_hdr(req, "Connection", "close");

    if (code != 0) {
        const char *http_status = "500 Internal Server Error";
        if (code == 400) http_status = "400 Bad Request";
        else if (code == 401) http_status = "401 Unauthorized";
        else if (code == 404) http_status = "404 Not Found";
        else if (code == 502) http_status = "502 Bad Gateway";
        else if (code == 503) http_status = "503 Service Unavailable";
        httpd_resp_set_status(req, http_status);
    }
    esp_err_t res = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
    return res;
}

static bool auth_check(httpd_req_t *req)
{
    const app_config_t *cfg = app_config_get();
    if (!cfg || cfg->api_password[0] == '\0') {
        return true; /* 免密访问 */
    }

    /* 1. 检查 Authorization 头: "Bearer <token>" 或 "<token>" */
    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) == ESP_OK) {
        const char *token = auth_hdr;
        if (strncmp(token, "Bearer ", 7) == 0) {
            token += 7;
        }
        while (*token == ' ') token++;
        if (strcmp(token, cfg->api_password) == 0) {
            return true;
        }
    }

    /* 2. 检查 Query 参数: ?token=<token> */
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char token[64] = {0};
        if (httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK) {
            if (strcmp(token, cfg->api_password) == 0) {
                return true;
            }
        }
    }

    ESP_LOGW(TAG, "Unauthorized access attempt to %s", req->uri);
    send_json_resp(req, 401, "unauthorized", NULL);
    return false;
}

static esp_err_t read_body(httpd_req_t *req, char **out)
{
    *out = NULL;
    int total = req->content_len;
    if (total <= 0) return ESP_OK;
    if (total > 8192) return ESP_FAIL;
    char *buf = malloc(total + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int r = 0;
    while (r < total) {
        int x = httpd_req_recv(req, buf + r, total - r);
        if (x <= 0) { free(buf); return ESP_FAIL; }
        r += x;
    }
    buf[r] = 0;
    *out = buf;
    return ESP_OK;
}

/* ---------------- OPTIONS preflight handler ---------------- */

static esp_err_t h_options(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------- Root Service Info ---------------- */

static esp_err_t h_root_info(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "service", "esp32-hid-controller");
    cJSON_AddStringToObject(r, "status", "online");
    cJSON_AddStringToObject(r, "device_id", s_device_id);
    cJSON_AddBoolToObject  (r, "auth_required", app_config_get()->api_password[0] != '\0');
    return send_json_resp(req, 0, NULL, r);
}

/* ---------------- Auth Endpoints ---------------- */

static esp_err_t h_auth_login(httpd_req_t *req)
{
    char *body = NULL;
    if (read_body(req, &body) != ESP_OK || !body) {
        return send_json_resp(req, 400, "bad body", NULL);
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_json_resp(req, 400, "bad json", NULL);

    const cJSON *jpwd = cJSON_GetObjectItem(j, "password");
    const char *pwd = (jpwd && cJSON_IsString(jpwd)) ? jpwd->valuestring : "";
    const app_config_t *cfg = app_config_get();

    bool ok = (cfg->api_password[0] == '\0') || (strcmp(pwd, cfg->api_password) == 0);
    cJSON_Delete(j);

    if (ok) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddBoolToObject(res, "valid", true);
        cJSON_AddBoolToObject(res, "has_pwd", cfg->api_password[0] != '\0');
        return send_json_resp(req, 0, NULL, res);
    } else {
        return send_json_resp(req, 401, "invalid password", NULL);
    }
}

static esp_err_t h_auth_change_pwd(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char *body = NULL;
    if (read_body(req, &body) != ESP_OK || !body) {
        return send_json_resp(req, 400, "bad body", NULL);
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_json_resp(req, 400, "bad json", NULL);

    const cJSON *jnew = cJSON_GetObjectItem(j, "new_pwd");
    if (!jnew || !cJSON_IsString(jnew)) {
        cJSON_Delete(j);
        return send_json_resp(req, 400, "new_pwd missing", NULL);
    }

    app_config_t cfg = *app_config_get();
    strlcpy(cfg.api_password, jnew->valuestring, sizeof(cfg.api_password));
    app_config_save(&cfg);
    cJSON_Delete(j);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "saved", true);
    return send_json_resp(req, 0, NULL, res);
}

/* ---------------- Logs Endpoints ---------------- */

static esp_err_t h_logs_config(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char *body = NULL;
    if (read_body(req, &body) != ESP_OK || !body) {
        return send_json_resp(req, 400, "bad body", NULL);
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_json_resp(req, 400, "bad json", NULL);

    const cJSON *jen = cJSON_GetObjectItem(j, "enabled");
    if (jen && cJSON_IsBool(jen)) {
        log_capture_set_enabled(cJSON_IsTrue(jen));
    }
    cJSON_Delete(j);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "enabled", log_capture_is_enabled());
    return send_json_resp(req, 0, NULL, res);
}

static esp_err_t h_logs_clear(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    log_capture_clear();
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "cleared", true);
    return send_json_resp(req, 0, NULL, res);
}

static esp_err_t h_get_logs(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char query[64] = {0};
    uint32_t seq = 0;
    bool raw = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(query, "seq", param, sizeof(param)) == ESP_OK) {
            seq = (uint32_t)strtoul(param, NULL, 10);
        }
        if (httpd_query_key_value(query, "raw", param, sizeof(param)) == ESP_OK) {
            raw = (strcmp(param, "1") == 0 || strcmp(param, "true") == 0);
        }
    }

    char *buf = malloc(4096);
    if (!buf) {
        return send_json_resp(req, 500, "out of memory", NULL);
    }

    size_t n = log_capture_read(buf, 4095, &seq);
    buf[n] = '\0';

    if (raw) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        set_cors_headers(req);
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, buf, n);
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "enabled", log_capture_is_enabled());
        cJSON_AddNumberToObject(root, "seq", seq);
        cJSON_AddStringToObject(root, "logs", buf);
        send_json_resp(req, 0, NULL, root);
    }
    free(buf);
    return ESP_OK;
}

/* ---------------- Dynamic Macro Endpoints ---------------- */

static esp_err_t h_macros_get(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    cJSON *list = macro_mgr_get_all();
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "macros", list ? list : cJSON_CreateArray());
    return send_json_resp(req, 0, NULL, res);
}

static esp_err_t h_macros_save(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    char *body = NULL;
    if (read_body(req, &body) != ESP_OK || !body) {
        return send_json_resp(req, 400, "bad body", NULL);
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_json_resp(req, 400, "bad json", NULL);

    esp_err_t err = macro_mgr_save(j);
    cJSON_Delete(j);
    if (err != ESP_OK) {
        return send_json_resp(req, 500, "save failed", NULL);
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "saved", true);
    return send_json_resp(req, 0, NULL, res);
}

static esp_err_t h_macros_delete(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    char *body = NULL;
    if (read_body(req, &body) != ESP_OK || !body) {
        return send_json_resp(req, 400, "bad body", NULL);
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_json_resp(req, 400, "bad json", NULL);

    const cJSON *jid = cJSON_GetObjectItem(j, "id");
    if (!jid || !cJSON_IsString(jid)) {
        cJSON_Delete(j);
        return send_json_resp(req, 400, "id missing", NULL);
    }
    esp_err_t err = macro_mgr_delete(jid->valuestring);
    cJSON_Delete(j);
    if (err != ESP_OK) {
        return send_json_resp(req, 404, "macro not found", NULL);
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "deleted", true);
    return send_json_resp(req, 0, NULL, res);
}

static esp_err_t h_macros_exec(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    char *body = NULL;
    if (read_body(req, &body) != ESP_OK || !body) {
        return send_json_resp(req, 400, "bad body", NULL);
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_json_resp(req, 400, "bad json", NULL);

    const cJSON *jid = cJSON_GetObjectItem(j, "id");
    if (!jid || !cJSON_IsString(jid)) {
        cJSON_Delete(j);
        return send_json_resp(req, 400, "id missing", NULL);
    }
    char errmsg[64] = {0};
    int code = macro_mgr_exec(jid->valuestring, errmsg, sizeof(errmsg));
    cJSON_Delete(j);

    if (code == 0) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddBoolToObject(res, "executed", true);
        return send_json_resp(req, 0, NULL, res);
    } else {
        return send_json_resp(req, code, errmsg, NULL);
    }
}

/* ---------------- System Reset / Bootloader Endpoints ---------------- */

static void deferred_bootloader_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    hid_usb_reboot_bootloader();
    vTaskDelete(NULL);
}

static void deferred_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    hid_usb_reboot_system();
    vTaskDelete(NULL);
}

static esp_err_t h_system_bootloader(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "reboot_to_bootloader", true);
    send_json_resp(req, 0, NULL, res);
    xTaskCreate(deferred_bootloader_task, "def_boot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_system_reboot(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "reboot", true);
    send_json_resp(req, 0, NULL, res);
    xTaskCreate(deferred_reboot_task, "def_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---------------- Direct Stream OTA Upload ---------------- */

static void delayed_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t h_ota_upload(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        return send_json_resp(req, 500, "no ota partition", NULL);
    }

    ESP_LOGI(TAG, "Starting OTA stream upload to %s (%d bytes)", update_partition->label, req->content_len);
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: 0x%x", err);
        return send_json_resp(req, 500, "ota begin failed", NULL);
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        return send_json_resp(req, 500, "oom", NULL);
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > 4096 ? 4096 : remaining;
        int ret = httpd_req_recv(req, buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "recv failed in ota upload");
            return send_json_resp(req, 500, "recv failed", NULL);
        }
        err = esp_ota_write(ota_handle, buf, ret);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "esp_ota_write failed: 0x%x", err);
            return send_json_resp(req, 500, "ota write failed", NULL);
        }
        remaining -= ret;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: 0x%x", err);
        return send_json_resp(req, 500, "ota end failed", NULL);
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: 0x%x", err);
        return send_json_resp(req, 500, "set boot partition failed", NULL);
    }

    ESP_LOGI(TAG, "OTA upgrade successful! Partition set to %s. Rebooting...", update_partition->label);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "success", true);
    cJSON_AddStringToObject(res, "partition", update_partition->label);
    send_json_resp(req, 0, NULL, res);

    xTaskCreate(delayed_reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---------------- WebSocket HID handler ---------------- */

static esp_err_t h_ws_hid(httpd_req_t *req)
{
    /* 握手时鉴权 */
    if (req->method == HTTP_GET) {
        if (!auth_check(req)) {
            return ESP_FAIL;
        }
    }
    return ws_hid_handle_frame(req);
}

/* ---------------- Generic Command Router Handler ---------------- */

typedef struct {
    const char *path;
    httpd_method_t method;
    const char *cmd_type;
} route_t;

static const route_t ROUTES[] = {
    { "/api/status",          HTTP_GET,  "get_status"     },
    { "/api/wifi/status",     HTTP_GET,  "wifi.status"    },
    { "/api/wifi/scan",       HTTP_GET,  "wifi.scan"      },
    { "/api/wifi/connect",    HTTP_POST, "wifi.connect"   },
    { "/api/ble/info",        HTTP_GET,  "ble.info"       },
    { "/api/ble/bonds",       HTTP_GET,  "ble.list_bonds" },
    { "/api/ble/unpair",      HTTP_POST, "ble.unpair"     },
    { "/api/ble/unpair_all",  HTTP_POST, "ble.unpair_all" },
    { "/api/ble/disconnect",  HTTP_POST, "ble.disconnect" },
    { "/api/ble/switch",      HTTP_POST, "ble.switch"     },
    { "/api/ble/scan",        HTTP_POST, "ble.scan"       },
    { "/api/usb/info",        HTTP_GET,  "usb.info"       },
    { "/api/transport",       HTTP_GET,  "transport.get"  },
    { "/api/transport/set",   HTTP_POST, "transport.set"  },
    { "/api/action",          HTTP_POST, "action"         },
    { "/api/reboot",          HTTP_POST, "reboot"         },
    /* 语义化控制与电源脉冲 */
    { "/api/cmd/shutdown",    HTTP_POST, "cmd.shutdown"   },
    { "/api/cmd/lock",        HTTP_POST, "cmd.lock"       },
    { "/api/cmd/desktop",     HTTP_POST, "cmd.desktop"    },
    { "/api/cmd/mute",        HTTP_POST, "cmd.mute"       },
    { "/api/cmd/volup",       HTTP_POST, "cmd.volup"      },
    { "/api/cmd/voldown",     HTTP_POST, "cmd.voldown"    },
    { "/api/cmd/playpause",   HTTP_POST, "cmd.playpause"  },
    { "/api/cmd/power_pulse", HTTP_POST, "power.pulse"    },
    { "/api/ota/start",       HTTP_POST, "ota.start"      },
    { "/api/ota/status",      HTTP_GET,  "ota.status"     },
};
#define N_ROUTES (sizeof(ROUTES)/sizeof(ROUTES[0]))

static esp_err_t generic_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    const route_t *route = (const route_t *)req->user_ctx;
    cJSON *payload = NULL;
    char *body = NULL;
    if (route->method == HTTP_POST) {
        if (read_body(req, &body) != ESP_OK) {
            send_json_resp(req, 400, "bad body", NULL);
            return ESP_OK;
        }
        if (body && body[0]) {
            payload = cJSON_Parse(body);
            if (!payload) {
                free(body);
                send_json_resp(req, 400, "bad json", NULL);
                return ESP_OK;
            }
        }
    }

    char err[96] = {0};
    cJSON *result = cJSON_CreateObject();
    if (strcmp(route->cmd_type, "get_status") != 0) {
        ESP_LOGI(TAG, "HTTP %s -> %s", route->path, route->cmd_type);
    }
    int code = cmd_router_dispatch(route->cmd_type, payload, result, err, sizeof(err));

    if (code == 0 && result->child != NULL) {
        send_json_resp(req, 0, NULL, result);
    } else {
        cJSON_Delete(result);
        send_json_resp(req, code, code == 0 ? NULL : err, NULL);
    }

    if (payload) cJSON_Delete(payload);
    free(body);
    return ESP_OK;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char *body = NULL;
    if (read_body(req, &body) != ESP_OK || !body) {
        send_json_resp(req, 400, "bad body", NULL);
        free(body);
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_json_resp(req, 400, "bad json", NULL); return ESP_OK; }
    const cJSON *jtype = cJSON_GetObjectItem(root, "type");
    const cJSON *jpayload = cJSON_GetObjectItem(root, "payload");
    if (!jtype || !cJSON_IsString(jtype)) {
        cJSON_Delete(root);
        send_json_resp(req, 400, "type missing", NULL);
        return ESP_OK;
    }
    char err[96] = {0};
    cJSON *result = cJSON_CreateObject();
    if (strcmp(jtype->valuestring, "ota.status") != 0) {
        ESP_LOGI(TAG, "HTTP /api/cmd -> %s", jtype->valuestring);
    }
    int code = cmd_router_dispatch(jtype->valuestring, jpayload, result, err, sizeof(err));
    if (code == 0 && result->child != NULL) {
        send_json_resp(req, 0, NULL, result);
    } else {
        cJSON_Delete(result);
        send_json_resp(req, code, code == 0 ? NULL : err, NULL);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---------------- mDNS & UDP discovery ---------------- */

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: 0x%x", err);
        return;
    }
    mdns_hostname_set("esp32-hid");
    mdns_instance_name_set("ESP32 HID Controller");

    mdns_service_add("esp32_http", "_http", "_tcp", HTTP_PORT, NULL, 0);
    mdns_service_add("esp32_hid",  "_hid",  "_tcp", HTTP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS active: http://esp32-hid.local");
}

static void udp_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "udp socket fail"); vTaskDelete(NULL); return; }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DISCOVERY_PORT);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "udp bind fail");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UDP discovery listening on :%d", DISCOVERY_PORT);

    char buf[256];
    while (s_udp_run) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&src, &sl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strncmp(buf, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC)) != 0) continue;

        char ip[16] = {0};
        wifi_mgr_get_ip(ip, sizeof(ip));
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "magic", "ESP32_HID");
        cJSON_AddStringToObject(r, "device_id", s_device_id);
        cJSON_AddStringToObject(r, "ip", ip);
        cJSON_AddNumberToObject(r, "http_port", HTTP_PORT);
        cJSON_AddBoolToObject  (r, "auth_required", app_config_get()->api_password[0] != '\0');
        char *s = cJSON_PrintUnformatted(r);
        if (s) {
            sendto(sock, s, strlen(s), 0, (struct sockaddr*)&src, sl);
            ESP_LOGI(TAG, "discovery reply to %s", inet_ntoa(src.sin_addr));
            free(s);
        }
        cJSON_Delete(r);
    }
    close(sock);
    s_udp_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- Public Lifecycle ---------------- */

esp_err_t lan_mode_start(const char *device_id)
{
    if (s_httpd) return ESP_OK;
    if (device_id) strlcpy(s_device_id, device_id, sizeof(s_device_id));

    macro_mgr_init();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = HTTP_PORT;
    hc.max_uri_handlers = N_ROUTES + 24;
    hc.lru_purge_enable = true;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return ESP_FAIL;
    }

    /* 1. OPTIONS preflight wildcard / common paths */
    httpd_uri_t u_opt = { .uri="/api/*", .method=HTTP_OPTIONS, .handler=h_options };
    httpd_register_uri_handler(s_httpd, &u_opt);

    /* 2. Root service info */
    httpd_uri_t u_root = { .uri="/", .method=HTTP_GET, .handler=h_root_info };
    httpd_register_uri_handler(s_httpd, &u_root);

    /* 3. Auth routes */
    httpd_uri_t u_login = { .uri="/api/auth/login", .method=HTTP_POST, .handler=h_auth_login };
    httpd_register_uri_handler(s_httpd, &u_login);
    httpd_uri_t u_chg_pwd = { .uri="/api/auth/change_pwd", .method=HTTP_POST, .handler=h_auth_change_pwd };
    httpd_register_uri_handler(s_httpd, &u_chg_pwd);

    /* 4. Logs routes */
    httpd_uri_t u_logs_cfg = { .uri="/api/logs/config", .method=HTTP_POST, .handler=h_logs_config };
    httpd_register_uri_handler(s_httpd, &u_logs_cfg);
    httpd_uri_t u_logs_clr = { .uri="/api/logs/clear", .method=HTTP_POST, .handler=h_logs_clear };
    httpd_register_uri_handler(s_httpd, &u_logs_clr);
    httpd_uri_t u_logs = { .uri="/api/logs", .method=HTTP_GET, .handler=h_get_logs };
    httpd_register_uri_handler(s_httpd, &u_logs);

    /* 5. Dynamic Macros routes */
    httpd_uri_t u_mac_get = { .uri="/api/macros", .method=HTTP_GET, .handler=h_macros_get };
    httpd_register_uri_handler(s_httpd, &u_mac_get);
    httpd_uri_t u_mac_sav = { .uri="/api/macros", .method=HTTP_POST, .handler=h_macros_save };
    httpd_register_uri_handler(s_httpd, &u_mac_sav);
    httpd_uri_t u_mac_del = { .uri="/api/macros/delete", .method=HTTP_POST, .handler=h_macros_delete };
    httpd_register_uri_handler(s_httpd, &u_mac_del);
    httpd_uri_t u_mac_exe = { .uri="/api/macros/exec", .method=HTTP_POST, .handler=h_macros_exec };
    httpd_register_uri_handler(s_httpd, &u_mac_exe);

    /* 6. OTA stream direct upload */
    httpd_uri_t u_ota_up = { .uri="/api/ota/upload", .method=HTTP_POST, .handler=h_ota_upload };
    httpd_register_uri_handler(s_httpd, &u_ota_up);

    /* 7. WebSocket HID Passthrough */
    httpd_uri_t u_ws_hid = {
        .uri = "/ws/hid",
        .method = HTTP_GET,
        .handler = h_ws_hid,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(s_httpd, &u_ws_hid);

    /* 8. System Reboot & Bootloader routes */
    httpd_uri_t u_sys_boot = { .uri="/api/system/bootloader", .method=HTTP_POST, .handler=h_system_bootloader };
    httpd_register_uri_handler(s_httpd, &u_sys_boot);
    httpd_uri_t u_sys_boot_g = { .uri="/api/system/bootloader", .method=HTTP_GET, .handler=h_system_bootloader };
    httpd_register_uri_handler(s_httpd, &u_sys_boot_g);
    httpd_uri_t u_sys_reb = { .uri="/api/system/reboot", .method=HTTP_POST, .handler=h_system_reboot };
    httpd_register_uri_handler(s_httpd, &u_sys_reb);
    httpd_uri_t u_sys_reb_g = { .uri="/api/system/reboot", .method=HTTP_GET, .handler=h_system_reboot };
    httpd_register_uri_handler(s_httpd, &u_sys_reb_g);

    /* 9. Generic router routes */
    for (size_t i = 0; i < N_ROUTES; i++) {
        httpd_uri_t u = {
            .uri = ROUTES[i].path,
            .method = ROUTES[i].method,
            .handler = generic_handler,
            .user_ctx = (void *)&ROUTES[i],
        };
        httpd_register_uri_handler(s_httpd, &u);
    }
    httpd_uri_t u_cmd = { .uri="/api/cmd", .method=HTTP_POST, .handler=cmd_handler };
    httpd_register_uri_handler(s_httpd, &u_cmd);

    ESP_LOGI(TAG, "Headless REST & WebSocket HID server up on :%d", HTTP_PORT);

    start_mdns();

    s_udp_run = true;
    xTaskCreate(udp_task, "udp_disc", 4096, NULL, 4, &s_udp_task);
    return ESP_OK;
}

void lan_mode_stop(void)
{
    s_udp_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    mdns_free();
}
