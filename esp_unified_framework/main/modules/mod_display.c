#include "mod_display.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "app_config.h"
#include "hal_display.h"
#include "hal_epd.h"
#if CONFIG_FEATURE_PC_MONITOR
#include "mod_pc_monitor.h"
#endif
#include "web_server.h"

static const char *TAG = "mod_display";

#if CONFIG_FEATURE_DISPLAY || CONFIG_FEATURE_PC_MONITOR
cJSON *mod_display_build_status_json(void)
{
#if CONFIG_FEATURE_PC_MONITOR
    return mod_pc_monitor_build_status_json();
#else
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    hal_display_populate_status_cjson(root);
    return root;
#endif
}

esp_err_t mod_display_update_json(const char *json_str, bool switch_text_on_lines)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
#if CONFIG_FEATURE_PC_MONITOR
    return mod_pc_monitor_update_display_json(json_str, switch_text_on_lines);
#else
    cJSON *parsed = cJSON_Parse(json_str);
    if (!parsed || !cJSON_IsObject(parsed)) {
        if (parsed) cJSON_Delete(parsed);
        return ESP_ERR_INVALID_ARG;
    }
    bool force_full = cJSON_IsTrue(cJSON_GetObjectItem(parsed, "full_refresh"));
    bool want_refresh = force_full || cJSON_IsTrue(cJSON_GetObjectItem(parsed, "refresh"));
    bool clear_only = cJSON_IsTrue(cJSON_GetObjectItem(parsed, "clear"));
    cJSON_Delete(parsed);

    app_config_t updated;
    app_config_get(&updated);
    bool changed = app_config_apply_display_json(json_str, &updated, switch_text_on_lines);
    if (changed) {
        updated.config_ver++;
        app_config_save(&updated);
    }
    if (changed || want_refresh || clear_only) {
        hal_display_apply_config_ex(force_full, clear_only && !want_refresh);
    }
    return ESP_OK;
#endif
}
esp_err_t mod_display_update_image_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
#if !CONFIG_FEATURE_PC_MONITOR && CONFIG_FEATURE_DISPLAY
    bool force_full = true;
    esp_err_t err = hal_epd_apply_image_json(json_str, &force_full);
    if (err != ESP_OK) return err;
    hal_display_apply_config_ex(force_full, false);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
#else
cJSON *mod_display_build_status_json(void) { return NULL; }
esp_err_t mod_display_update_json(const char *json_str, bool switch_text_on_lines)
{
    (void)json_str; (void)switch_text_on_lines;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t mod_display_update_image_json(const char *json_str)
{
    (void)json_str;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

#if CONFIG_FEATURE_DISPLAY

static void send_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t display_status_get_handler(httpd_req_t *req)
{
    if (!web_server_check_read_auth(req)) return ESP_FAIL;
    send_cors(req);
    cJSON *root = mod_display_build_status_json();
    char *out = root ? cJSON_PrintUnformatted(root) : NULL;
    if (root) cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (!out) return httpd_resp_sendstr(req, "{\"ok\":false}");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

static esp_err_t display_update_handler(httpd_req_t *req, bool switch_text_on_lines)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    char *body = calloc(1, len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
        return ESP_FAIL;
    }

    esp_err_t err = mod_display_update_json(body, switch_text_on_lines);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
        return ESP_FAIL;
    }
    return display_status_get_handler(req);
}

static esp_err_t display_text_post_handler(httpd_req_t *req)
{
    return display_update_handler(req, true);
}

static esp_err_t display_config_post_handler(httpd_req_t *req)
{
    return display_update_handler(req, false);
}

static esp_err_t display_image_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    int len = req->content_len;
    if (len <= 0 || len > 65536) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid image body length (max 64KB)");
        return ESP_FAIL;
    }

    char *body = calloc(1, (size_t)len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, body + received, len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    esp_err_t err = mod_display_update_image_json(body);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid EPD image payload");
        return ESP_FAIL;
    }
    return display_status_get_handler(req);
}

#if !CONFIG_FEATURE_PC_MONITOR
static void extract_kv_str(const char *buf, const char *prefix, char *dst, size_t cap)
{
    const char *p = buf;
    size_t plen = strlen(prefix);
    while ((p = strstr(p, prefix)) != NULL) {
        if (p == buf || *(p - 1) == ',') {
            p += plen;
            const char *end = strchr(p, ',');
            size_t l = end ? (size_t)(end - p) : strlen(p);
            if (l >= cap) l = cap - 1;
            memcpy(dst, p, l);
            dst[l] = '\0';
            return;
        }
        p += plen;
    }
}

static esp_err_t monitor_push_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char *body = calloc(1, len + 1);
    if (!body) return ESP_FAIL;
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) {
        free(body);
        return ESP_FAIL;
    }
    body[r] = '\0';

    float cpu = 0.0f, ram = 0.0f, disk = 0.0f;
    char dl[20] = "0K/s", ul[20] = "0K/s", up[16] = "--", tm[16] = "--:--";
    bool want_refresh = false;

    cJSON *root = cJSON_Parse(body);
    if (root && cJSON_IsObject(root)) {
        const cJSON *jc = cJSON_GetObjectItem(root, "cpu");
        const cJSON *jr = cJSON_GetObjectItem(root, "ram");
        const cJSON *jd = cJSON_GetObjectItem(root, "disk");
        const cJSON *jdl = cJSON_GetObjectItem(root, "down");
        const cJSON *jul = cJSON_GetObjectItem(root, "up");
        const cJSON *jup = cJSON_GetObjectItem(root, "uptime");
        const cJSON *jtm = cJSON_GetObjectItem(root, "time");
        if (cJSON_IsNumber(jc)) cpu = (float)jc->valuedouble;
        if (cJSON_IsNumber(jr)) ram = (float)jr->valuedouble;
        if (cJSON_IsNumber(jd)) disk = (float)jd->valuedouble;
        if (cJSON_IsString(jdl)) strlcpy(dl, jdl->valuestring, sizeof(dl));
        if (cJSON_IsString(jul)) strlcpy(ul, jul->valuestring, sizeof(ul));
        if (cJSON_IsString(jup)) strlcpy(up, jup->valuestring, sizeof(up));
        if (cJSON_IsString(jtm)) strlcpy(tm, jtm->valuestring, sizeof(tm));
        want_refresh = cJSON_IsTrue(cJSON_GetObjectItem(root, "refresh"));
        cJSON_Delete(root);
    } else {
        if (root) cJSON_Delete(root);
        const char *p = strstr(body, "C:");
        if (p) cpu = strtof(p + 2, NULL);
        p = strstr(body, "R:");
        if (p) ram = strtof(p + 2, NULL);
        p = body;
        while ((p = strstr(p, "D:")) != NULL) {
            if (p == body || *(p - 1) == ',') {
                disk = strtof(p + 2, NULL);
                break;
            }
            p += 2;
        }
        extract_kv_str(body, "DL:", dl, sizeof(dl));
        extract_kv_str(body, "UL:", ul, sizeof(ul));
        extract_kv_str(body, "UP:", up, sizeof(up));
        extract_kv_str(body, "T:", tm, sizeof(tm));
    }
    free(body);

    hal_epd_update_metrics(cpu, ram, disk, dl, ul, up, tm);
    if (want_refresh && strcmp(app_config_ptr()->display.mode, "monitor") == 0) {
        hal_epd_request_refresh(false, false);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}
#endif

esp_err_t mod_display_init(void)
{
    esp_err_t err = hal_display_init();
#if !CONFIG_FEATURE_PC_MONITOR
    hal_display_apply_config();
#endif
    return err;
}

esp_err_t mod_display_register_routes(httpd_handle_t server)
{
    httpd_uri_t u_st = { .uri = "/api/display/status", .method = HTTP_GET, .handler = display_status_get_handler };
    httpd_register_uri_handler(server, &u_st);

    httpd_uri_t u_tx = { .uri = "/api/display/text", .method = HTTP_POST, .handler = display_text_post_handler };
    httpd_register_uri_handler(server, &u_tx);

    httpd_uri_t u_cf = { .uri = "/api/display/config", .method = HTTP_POST, .handler = display_config_post_handler };
    httpd_register_uri_handler(server, &u_cf);

    httpd_uri_t u_img = { .uri = "/api/display/image", .method = HTTP_POST, .handler = display_image_post_handler };
    httpd_register_uri_handler(server, &u_img);

#if !CONFIG_FEATURE_PC_MONITOR
    httpd_uri_t u_mp = { .uri = "/api/monitor/push", .method = HTTP_POST, .handler = monitor_push_post_handler };
    httpd_register_uri_handler(server, &u_mp);
#endif

    ESP_LOGI(TAG, "Display REST routes registered (/api/display/status, /config, /text, /image)");
    return ESP_OK;
}

#else

esp_err_t mod_display_init(void) { return ESP_OK; }
esp_err_t mod_display_register_routes(httpd_handle_t server) { (void)server; return ESP_OK; }

#endif
