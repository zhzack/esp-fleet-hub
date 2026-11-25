#include "mod_display.h"
#include "sdkconfig.h"

#if CONFIG_FEATURE_DISPLAY

#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"
#include "hal_display.h"
#include "web_server.h"

static const char *TAG = "mod_display";

static void send_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t display_status_get_handler(httpd_req_t *req)
{
    send_cors(req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "ready", hal_display_is_ready());
    cJSON_AddStringToObject(root, "driver", hal_display_driver_name());
    cJSON_AddNumberToObject(root, "backlight", hal_display_get_backlight());
    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    for (int i = 0; i < HAL_DISPLAY_MAX_LINES; i++) {
        cJSON_AddItemToArray(lines, cJSON_CreateString(hal_display_get_line(i)));
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (!out) return httpd_resp_sendstr(req, "{\"ok\":false}");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

static esp_err_t display_text_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    int len = req->content_len;
    if (len <= 0 || len > 512) {
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

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
        return ESP_FAIL;
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "clear"))) {
        hal_display_clear();
    }
    const cJSON *bl = cJSON_GetObjectItem(root, "backlight");
    if (cJSON_IsNumber(bl)) {
        hal_display_set_backlight(bl->valueint);
    }
    for (int i = 0; i < HAL_DISPLAY_MAX_LINES; i++) {
        char k[8];
        snprintf(k, sizeof(k), "line%d", i);
        const cJSON *item = cJSON_GetObjectItem(root, k);
        if (cJSON_IsString(item)) {
            hal_display_set_line(i, item->valuestring);
        }
    }
    const cJSON *text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text)) {
        hal_display_set_line(0, text->valuestring);
    }

    cJSON_Delete(root);
    return display_status_get_handler(req);
}

esp_err_t mod_display_init(void)
{
    esp_err_t err = hal_display_init();
    hal_display_set_line(0, "ESP Unified Display");
    hal_display_set_line(1, hal_display_driver_name());
    return err;
}

esp_err_t mod_display_register_routes(httpd_handle_t server)
{
    httpd_uri_t u_st = { .uri = "/api/display/status", .method = HTTP_GET, .handler = display_status_get_handler };
    httpd_register_uri_handler(server, &u_st);

    httpd_uri_t u_tx = { .uri = "/api/display/text", .method = HTTP_POST, .handler = display_text_post_handler };
    httpd_register_uri_handler(server, &u_tx);

    ESP_LOGI(TAG, "Display REST routes registered (/api/display/status, /api/display/text)");
    return ESP_OK;
}

#else

esp_err_t mod_display_init(void) { return ESP_OK; }
esp_err_t mod_display_register_routes(httpd_handle_t server) { return ESP_OK; }

#endif
