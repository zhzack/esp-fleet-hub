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
#include "ota_mgr.h"
#include "web_ui_html.h"

#if CONFIG_FEATURE_GPIO_CTRL
#include "mod_gpio.h"
#endif

#if CONFIG_FEATURE_CAMERA
#include "mod_camera.h"
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, PAGE_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

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

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    // Allocate 8KB buffer for recent logs
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

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    app_config_t cfg;
    app_config_get(&cfg);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = cfg.web_port ? cfg.web_port : 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP Web Portal on port %d...", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return ESP_FAIL;
    }

    // 1. Root and UI routes
    httpd_uri_t uri_root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_index = {
        .uri       = "/index.html",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(s_server, &uri_index);

    // 2. Core System REST routes
    httpd_uri_t uri_caps = {
        .uri       = "/api/system/capabilities",
        .method    = HTTP_GET,
        .handler   = caps_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(s_server, &uri_caps);

    httpd_uri_t uri_status = {
        .uri       = "/api/system/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_logs = {
        .uri       = "/api/system/logs",
        .method    = HTTP_GET,
        .handler   = logs_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(s_server, &uri_logs);

    httpd_uri_t uri_reboot = {
        .uri       = "/api/system/reboot",
        .method    = HTTP_POST,
        .handler   = reboot_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(s_server, &uri_reboot);

    // 3. Dual-OTA Upload route
    httpd_uri_t uri_ota = {
        .uri       = "/api/ota/upload",
        .method    = HTTP_POST,
        .handler   = ota_upload_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota);

    // 4. Conditional Feature Modules
#if CONFIG_FEATURE_GPIO_CTRL
    mod_gpio_register_routes(s_server);
#endif

#if CONFIG_FEATURE_CAMERA
    mod_camera_register_routes(s_server);
#endif

    ESP_LOGI(TAG, "HTTP Web Server and API routes started successfully.");
    return ESP_OK;
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
