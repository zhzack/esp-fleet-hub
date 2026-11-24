#include "mod_camera.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "hal_camera.h"
#include "hal_gpio.h"

static const char *TAG = "mod_cam";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req)
{
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
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    int duty = 0;
    char *p = strstr(buf, "\"duty\":");
    if (p) duty = atoi(p + 7);

    hal_camera_set_flash_duty((uint8_t)duty);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"flash_duty\":%d}", duty);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
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

    ESP_LOGI(TAG, "Camera streaming routes registered (/stream, /capture, /api/camera/flash)");
    return ESP_OK;
}
