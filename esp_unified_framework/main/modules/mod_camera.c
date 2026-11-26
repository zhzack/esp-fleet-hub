#include "mod_camera.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "hal_camera.h"
#include "hal_gpio.h"
#include "app_config.h"
#include "web_server.h"

static const char *TAG = "mod_cam";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!web_server_check_read_auth(req)) return ESP_FAIL;
    if (!hal_camera_is_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera hardware not initialized");
        return ESP_FAIL;
    }

    esp_err_t res = ESP_OK;
    char part_buf[128];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "25");

    while (true) {
        camera_fb_t *fb = hal_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera frame capture failed");
            res = ESP_FAIL;
            break;
        }

        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        hal_camera_fb_return(fb);

        if (res != ESP_OK) {
            // Client disconnected
            break;
        }
    }

    return res;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!web_server_check_read_auth(req)) return ESP_FAIL;
    if (!hal_camera_is_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera not ready");
        return ESP_FAIL;
    }

    camera_fb_t *fb = hal_camera_fb_get();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Frame grab failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    hal_camera_fb_return(fb);
    return res;
}

static esp_err_t flash_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;

    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    int duty = 0;
    cJSON *root = cJSON_Parse(buf);
    if (root && cJSON_IsObject(root)) {
        const cJSON *j_duty = cJSON_GetObjectItem(root, "duty");
        if (cJSON_IsNumber(j_duty)) {
            duty = j_duty->valueint;
        }
        cJSON_Delete(root);
    } else {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    hal_camera_set_flash_duty((uint8_t)duty);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"flash_duty\":%d}", duty);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t camera_config_get_handler(httpd_req_t *req)
{
    if (!web_server_check_read_auth(req)) return ESP_FAIL;
    app_config_t cfg;
    app_config_get(&cfg);
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"framesize\":%u,\"quality\":%u,\"vflip\":%s,\"hmirror\":%s,\"brightness\":%d}",
             cfg.cam_framesize, cfg.cam_quality,
             cfg.cam_vflip ? "true" : "false",
             cfg.cam_hmirror ? "true" : "false",
             cfg.cam_brightness);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t camera_config_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    app_config_t cfg;
    app_config_get(&cfg);

    const cJSON *fs = cJSON_GetObjectItem(root, "framesize");
    const cJSON *q  = cJSON_GetObjectItem(root, "quality");
    const cJSON *vf = cJSON_GetObjectItem(root, "vflip");
    const cJSON *hm = cJSON_GetObjectItem(root, "hmirror");
    const cJSON *br = cJSON_GetObjectItem(root, "brightness");

    if (cJSON_IsNumber(fs)) cfg.cam_framesize = (uint8_t)fs->valueint;
    if (cJSON_IsNumber(q) && q->valueint >= 4 && q->valueint <= 63) cfg.cam_quality = (uint8_t)q->valueint;
    if (cJSON_IsBool(vf)) cfg.cam_vflip = cJSON_IsTrue(vf);
    if (cJSON_IsBool(hm)) cfg.cam_hmirror = cJSON_IsTrue(hm);
    if (cJSON_IsNumber(br)) cfg.cam_brightness = (int8_t)br->valueint;
    cfg.config_ver++;
    cJSON_Delete(root);

    app_config_save(&cfg);
    hal_camera_apply_config(cfg.cam_framesize, cfg.cam_quality, cfg.cam_vflip, cfg.cam_hmirror, cfg.cam_brightness);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Camera settings applied & saved\"}");
}

esp_err_t mod_camera_init(void)
{
    return hal_camera_init();
}

esp_err_t mod_camera_register_routes(httpd_handle_t server)
{
    httpd_uri_t uri_stream = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_stream);

    httpd_uri_t uri_capture = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_capture);

    httpd_uri_t uri_flash = {
        .uri       = "/api/camera/flash",
        .method    = HTTP_POST,
        .handler   = flash_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_flash);

    // Backward-compatible route alias for standalone esp32_cam clients
    httpd_uri_t uri_flash_compat = {
        .uri       = "/api/flash",
        .method    = HTTP_POST,
        .handler   = flash_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_flash_compat);

    httpd_uri_t uri_cam_cfg_get = {
        .uri       = "/api/camera/config",
        .method    = HTTP_GET,
        .handler   = camera_config_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_cam_cfg_get);

    httpd_uri_t uri_cam_cfg_post = {
        .uri       = "/api/camera/config",
        .method    = HTTP_POST,
        .handler   = camera_config_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_cam_cfg_post);

    httpd_uri_t uri_fsize_compat = {
        .uri       = "/api/framesize",
        .method    = HTTP_POST,
        .handler   = camera_config_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_fsize_compat);

    ESP_LOGI(TAG, "Camera routes registered (/stream, /capture, /api/camera/*, compat /api/flash, /api/framesize)");
    return ESP_OK;
}
