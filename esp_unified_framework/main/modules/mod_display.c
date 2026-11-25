#include "mod_display.h"
#include "sdkconfig.h"

#if CONFIG_FEATURE_DISPLAY

#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"
#include "app_config.h"
#include "hal_display.h"
#if CONFIG_FEATURE_PC_MONITOR
#include "mod_pc_monitor.h"
#endif
#include "web_server.h"

static const char *TAG = "mod_display";

static void send_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t display_status_get_handler(httpd_req_t *req)
{
    send_cors(req);
#if CONFIG_FEATURE_PC_MONITOR
    cJSON *root = mod_pc_monitor_build_status_json();
#else
    const app_display_config_t *dcfg = &app_config_ptr()->display;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "ready", hal_display_is_ready());
    cJSON_AddStringToObject(root, "driver", hal_display_driver_name());
    app_config_display_to_cjson(dcfg, root);
#endif
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

#if CONFIG_FEATURE_PC_MONITOR
    esp_err_t err = mod_pc_monitor_update_display_json(body, switch_text_on_lines);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
        return ESP_FAIL;
    }
#else
    app_config_t updated;
    app_config_get(&updated);
    bool changed = app_config_apply_display_json(body, &updated, switch_text_on_lines);
    free(body);
    if (changed) {
        updated.config_ver++;
        app_config_save(&updated);
    }
    hal_display_apply_config();
#endif
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

    ESP_LOGI(TAG, "Display REST routes registered (/api/display/status, /api/display/config, /api/display/text)");
    return ESP_OK;
}

#else

esp_err_t mod_display_init(void) { return ESP_OK; }
esp_err_t mod_display_register_routes(httpd_handle_t server) { return ESP_OK; }

#endif
