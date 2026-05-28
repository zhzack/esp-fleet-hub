#include "lan_mode.h"
#include "cmd_router.h"
#include "wifi_mgr.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define TAG "LAN"
#define DISCOVERY_PORT 42424
#define HTTP_PORT 80
#define DISCOVERY_MAGIC "ESP32_HID_DISCOVER"

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t   s_udp_task = NULL;
static volatile bool  s_udp_run = false;
static char           s_device_id[33] = {0};

/* ---------------- route table ---------------- */

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
};
#define N_ROUTES (sizeof(ROUTES)/sizeof(ROUTES[0]))

/* ---------------- helpers ---------------- */

static esp_err_t read_body(httpd_req_t *req, char **out)
{
    *out = NULL;
    int total = req->content_len;
    if (total <= 0) return ESP_OK;
    if (total > 4096) return ESP_FAIL;
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

static void send_json_resp(httpd_req_t *req, int code, const char *err, cJSON *result_owned)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", code == 0);
    cJSON_AddNumberToObject(root, "code", code);
    if (err && err[0]) cJSON_AddStringToObject(root, "error", err);
    if (result_owned) cJSON_AddItemToObject(root, "result", result_owned);
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (code != 0) {
        const char *http_status = "500 Internal Server Error";
        if (code == 400) http_status = "400 Bad Request";
        else if (code == 404) http_status = "404 Not Found";
        else if (code == 502) http_status = "502 Bad Gateway";
        else if (code == 503) http_status = "503 Service Unavailable";
        httpd_resp_set_status(req, http_status);
    }
    httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    cJSON_Delete(root);
}

static esp_err_t generic_handler(httpd_req_t *req)
{
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

/* ---------------- UDP discovery ---------------- */

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

/* ---------------- public ---------------- */

esp_err_t lan_mode_start(const char *device_id)
{
    if (s_httpd) return ESP_OK;
    if (device_id) strlcpy(s_device_id, device_id, sizeof(s_device_id));

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = HTTP_PORT;
    hc.max_uri_handlers = N_ROUTES + 4;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return ESP_FAIL;
    }
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
    ESP_LOGI(TAG, "HTTP server up on :%d (%u routes)", HTTP_PORT, (unsigned)N_ROUTES);

    s_udp_run = true;
    xTaskCreate(udp_task, "udp_disc", 4096, NULL, 4, &s_udp_task);
    return ESP_OK;
}

void lan_mode_stop(void)
{
    s_udp_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
}
