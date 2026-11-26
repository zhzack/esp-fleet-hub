#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "system_caps.h"
#include "app_config.h"
#include "log_capture.h"
#include "wifi_mgr.h"

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
#include "hal_camera.h"
#endif

#if CONFIG_FEATURE_PC_MONITOR
#include "mod_pc_monitor.h"
#endif

#if CONFIG_FEATURE_STORAGE
#include "mod_storage.h"
#endif

#if CONFIG_FEATURE_DISPLAY
#include "mod_display.h"
#include "hal_display.h"
#endif

#if CONFIG_FEATURE_USB_OTG
#include "mod_usb_otg.h"
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-API-Key, Authorization");
}

bool web_server_check_auth(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_get(&cfg);
    if (cfg.api_token[0] == '\0') {
        return true; // Open mode when no api_token is configured
    }

    char provided[64] = {0};
    if (httpd_req_get_hdr_value_len(req, "X-API-Key") > 0) {
        httpd_req_get_hdr_value_str(req, "X-API-Key", provided, sizeof(provided));
    } else if (httpd_req_get_hdr_value_len(req, "Authorization") > 0) {
        char auth_hdr[80] = {0};
        httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr));
        if (strncasecmp(auth_hdr, "Bearer ", 7) == 0) {
            strlcpy(provided, auth_hdr + 7, sizeof(provided));
        }
    } else {
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < 192) {
            char qbuf[192] = {0};
            if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
                httpd_query_key_value(qbuf, "token", provided, sizeof(provided));
            }
        }
    }

    if (strcmp(provided, cfg.api_token) == 0) {
        return true;
    }

    set_cors_headers(req);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\",\"message\":\"Valid X-API-Key or Bearer token required\"}");
    return false;
}

#if CONFIG_FEATURE_HTTP_SERVER

static esp_err_t cors_options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t captive_probe_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (wifi_mgr_is_ap_active()) {
        return captive_probe_handler(req);
    }
    set_cors_headers(req);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"endpoint_not_enabled\"}");
    return ESP_OK;
}

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
    char buf[1536];
    size_t len = system_caps_to_json(buf, sizeof(buf));
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[512];
    size_t len = system_status_to_json(buf, sizeof(buf));
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t len = wifi_mgr_scan(buf, 2048);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

static esp_err_t wifi_ap_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    bool enable = true;
    if (ret > 0) {
        buf[ret] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            const cJSON *j_en = cJSON_GetObjectItem(root, "enable");
            if (cJSON_IsBool(j_en)) enable = cJSON_IsTrue(j_en);
            cJSON_Delete(root);
        }
    }
    if (enable) wifi_mgr_start_softap();
    else wifi_mgr_stop_softap();

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, enable ? "{\"ok\":true,\"ap_active\":true}" : "{\"ok\":true,\"ap_active\":false}");
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char *buf = malloc(1536);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    size_t len = app_config_to_json(buf, 1536);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

static void wifi_switch_async_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(600)); // Wait so HTTP 200 response flushes cleanly first
    app_config_t cfg;
    app_config_get(&cfg);
    wifi_mgr_apply_new_sta(cfg.wifi_ssid, cfg.wifi_pass);
    vTaskDelete(NULL);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;

    int len = req->content_len;
    if (len <= 0 || len > 1535) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    app_config_changes_t changes = {0};
    esp_err_t err = app_config_update_from_json_ex(buf, &changes);
    free(buf);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"Invalid JSON or NVS save failure\"}");
    }

    app_config_t cfg;
    app_config_get(&cfg);

    if (changes.mdns_changed) {
        wifi_mgr_update_mdns(cfg.mdns_host, cfg.device_id, cfg.web_port);
    }
#if CONFIG_FEATURE_CAMERA
    if (changes.camera_changed) {
        hal_camera_apply_config(cfg.cam_framesize, cfg.cam_quality, cfg.cam_vflip, cfg.cam_hmirror, cfg.cam_brightness);
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
    if (changes.wifi_changed) {
        xTaskCreate(wifi_switch_async_task, "wifi_sw", 3072, NULL, 5, NULL);
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"config_ver\":%lu,\"wifi_switching\":%s,\"restart_required\":%s,\"message\":\"Configuration saved and applied\"}",
             (unsigned long)cfg.config_ver,
             changes.wifi_changed ? "true" : "false",
             changes.port_changed ? "true" : "false");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;

    char *buf = malloc(8192);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t len = log_capture_read(buf, 8192);
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
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
    if (!web_server_check_auth(req)) return ESP_FAIL;
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Device restarting...\"}");
    xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    app_config_factory_reset();
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Factory reset complete, rebooting...\"}");
    xTaskCreate(reboot_task, "web_reset", 2048, NULL, 5, NULL);
    return ESP_OK;
}
#endif // CONFIG_FEATURE_REST_API

#if CONFIG_FEATURE_OTA
static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    char buf[640];
    size_t len = ota_mgr_status_to_json(buf, sizeof(buf));
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t ota_upload_auth_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    set_cors_headers(req);
    return ota_upload_post_handler(req);
}

static esp_err_t ota_pull_auth_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    set_cors_headers(req);
    return ota_pull_post_handler(req);
}
#endif

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
    config.max_uri_handlers = 42;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server on port %d...", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return ESP_FAIL;
    }

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, http_404_error_handler);

#if CONFIG_FEATURE_WEB_UI
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_uri_t uri_idx = { .uri = "/index.html", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_server, &uri_idx);
    ESP_LOGI(TAG, "  -> Web Dashboard UI mounted at http://<ip>/");
#endif

#if CONFIG_FEATURE_REST_API
    httpd_uri_t uri_caps = { .uri = "/api/system/capabilities", .method = HTTP_GET, .handler = caps_get_handler };
    httpd_register_uri_handler(s_server, &uri_caps);

    httpd_uri_t uri_status = { .uri = "/api/system/status", .method = HTTP_GET, .handler = status_get_handler };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_status_compat = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
    httpd_register_uri_handler(s_server, &uri_status_compat);

    httpd_uri_t uri_cfg_get = { .uri = "/api/system/config", .method = HTTP_GET, .handler = config_get_handler };
    httpd_register_uri_handler(s_server, &uri_cfg_get);

    httpd_uri_t uri_cfg_post = { .uri = "/api/system/config", .method = HTTP_POST, .handler = config_post_handler };
    httpd_register_uri_handler(s_server, &uri_cfg_post);

    httpd_uri_t uri_logs = { .uri = "/api/system/logs", .method = HTTP_GET, .handler = logs_get_handler };
    httpd_register_uri_handler(s_server, &uri_logs);

    httpd_uri_t uri_reboot = { .uri = "/api/system/reboot", .method = HTTP_POST, .handler = reboot_post_handler };
    httpd_register_uri_handler(s_server, &uri_reboot);

    httpd_uri_t uri_reset = { .uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = factory_reset_post_handler };
    httpd_register_uri_handler(s_server, &uri_reset);

    httpd_uri_t uri_wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get_handler };
    httpd_register_uri_handler(s_server, &uri_wifi_scan);

    httpd_uri_t uri_wifi_ap = { .uri = "/api/wifi/ap", .method = HTTP_POST, .handler = wifi_ap_post_handler };
    httpd_register_uri_handler(s_server, &uri_wifi_ap);

    ESP_LOGI(TAG, "  -> REST Configuration API mounted (/api/system/*, /api/wifi/*)");
#endif

#if CONFIG_FEATURE_OTA
    httpd_uri_t uri_ota = { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = ota_upload_auth_handler };
    httpd_register_uri_handler(s_server, &uri_ota);

    httpd_uri_t uri_ota_pull = { .uri = "/api/ota/pull", .method = HTTP_POST, .handler = ota_pull_auth_handler };
    httpd_register_uri_handler(s_server, &uri_ota_pull);

    httpd_uri_t uri_ota_st = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_get_handler };
    httpd_register_uri_handler(s_server, &uri_ota_st);
    ESP_LOGI(TAG, "  -> Wireless OTA endpoints mounted (/api/ota/upload, /api/ota/pull, /api/ota/status)");
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

#if CONFIG_FEATURE_STORAGE
    mod_storage_register_routes(s_server);
#endif

#if CONFIG_FEATURE_DISPLAY
    mod_display_register_routes(s_server);
#endif

#if CONFIG_FEATURE_USB_OTG
    mod_usb_otg_register_routes(s_server);
#endif

    // Captive Portal OS probes
    httpd_uri_t uri_cap1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_probe_handler };
    httpd_register_uri_handler(s_server, &uri_cap1);
    httpd_uri_t uri_cap2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_probe_handler };
    httpd_register_uri_handler(s_server, &uri_cap2);
    httpd_uri_t uri_cap3 = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_probe_handler };
    httpd_register_uri_handler(s_server, &uri_cap3);

    // Wildcard CORS Preflight LAST
    httpd_uri_t uri_opts = { .uri = "/*", .method = HTTP_OPTIONS, .handler = cors_options_handler };
    httpd_register_uri_handler(s_server, &uri_opts);

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
