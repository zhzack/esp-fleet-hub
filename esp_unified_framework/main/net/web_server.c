#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "system_caps.h"
#include "app_config.h"
#include "log_capture.h"

#if CONFIG_FEATURE_OTA
#include "ota_mgr.h"
#endif

#if CONFIG_FEATURE_WEB_UI
#include "web_ui_html.h"
#endif

#if CONFIG_FEATURE_GPIO_CTRL
#include "mod_gpio.h"
#endif

#if CONFIG_FEATURE_CAMERA
#include "mod_camera.h"
#endif

#if CONFIG_FEATURE_PC_MONITOR
#include "mod_pc_monitor.h"
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#if CONFIG_FEATURE_HTTP_SERVER

#if CONFIG_FEATURE_WEB_UI
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, PAGE_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}
#endif

#if CONFIG_FEATURE_REST_API
static esp_err_t caps_get_handler(httpd_req_t *req)
{
    char buf[1024];
    size_t len = system_caps_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[512];
    size_t len = system_status_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char buf[512];
    size_t len = app_config_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    esp_err_t err = app_config_update_from_json(buf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Configuration saved to Flash NVS\"}");
    } else {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"Failed to save to Flash NVS\"}");
    }
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    char *buf = malloc(8192);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t len = log_capture_read(buf, 8192);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

static void reboot_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Device restarting...\"}");
    xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
#endif // CONFIG_FEATURE_REST_API

#endif // CONFIG_FEATURE_HTTP_SERVER

esp_err_t web_server_start(void)
{
#if !CONFIG_FEATURE_HTTP_SERVER
    ESP_LOGI(TAG, "[LuatOS Profile] HTTP Server disabled by profile configuration.");
    return ESP_OK;
#else
    if (s_server) return ESP_OK;

    app_config_t cfg;
    app_config_get(&cfg);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = cfg.web_port ? cfg.web_port : 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP Server on port %d...", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return ESP_FAIL;
    }

#if CONFIG_FEATURE_WEB_UI
    // Mount Responsive Web Dashboard
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_uri_t uri_idx = { .uri = "/index.html", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_server, &uri_idx);
    ESP_LOGI(TAG, "  -> Web Dashboard UI mounted at http://<ip>/");
#else
    ESP_LOGI(TAG, "  -> Headless Mode: Web UI dashboard omitted from firmware");
#endif

#if CONFIG_FEATURE_REST_API
    // Mount REST API Endpoints
    httpd_uri_t uri_caps = { .uri = "/api/system/capabilities", .method = HTTP_GET, .handler = caps_get_handler };
    httpd_register_uri_handler(s_server, &uri_caps);

    httpd_uri_t uri_status = { .uri = "/api/system/status", .method = HTTP_GET, .handler = status_get_handler };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_cfg_get = { .uri = "/api/system/config", .method = HTTP_GET, .handler = config_get_handler };
    httpd_register_uri_handler(s_server, &uri_cfg_get);

    httpd_uri_t uri_cfg_post = { .uri = "/api/system/config", .method = HTTP_POST, .handler = config_post_handler };
    httpd_register_uri_handler(s_server, &uri_cfg_post);

    httpd_uri_t uri_logs = { .uri = "/api/system/logs", .method = HTTP_GET, .handler = logs_get_handler };
    httpd_register_uri_handler(s_server, &uri_logs);

    httpd_uri_t uri_reboot = { .uri = "/api/system/reboot", .method = HTTP_POST, .handler = reboot_post_handler };
    httpd_register_uri_handler(s_server, &uri_reboot);
    ESP_LOGI(TAG, "  -> REST Configuration API mounted (/api/system/*)");
#endif

#if CONFIG_FEATURE_OTA
    httpd_uri_t uri_ota = { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = ota_upload_post_handler };
    httpd_register_uri_handler(s_server, &uri_ota);
    ESP_LOGI(TAG, "  -> Wireless OTA upload endpoint mounted (/api/ota/upload)");
#endif

#if CONFIG_FEATURE_GPIO_CTRL
    mod_gpio_register_routes(s_server);
#endif

#if CONFIG_FEATURE_CAMERA
    mod_camera_register_routes(s_server);
#endif

#if CONFIG_FEATURE_PC_MONITOR
    mod_pc_monitor_register_routes(s_server);
#endif

    return ESP_OK;
#endif // CONFIG_FEATURE_HTTP_SERVER
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}
