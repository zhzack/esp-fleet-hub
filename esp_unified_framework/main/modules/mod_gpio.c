#include "mod_gpio.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"
#include "hal_gpio.h"
#include "web_server.h"

static const char *TAG = "mod_gpio";

static esp_err_t gpio_pins_get_handler(httpd_req_t *req)
{
    char buf[1536];
    size_t len = hal_gpio_pins_to_json(buf, sizeof(buf));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t gpio_set_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;

    char content[256];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    content[recv_len] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
        return ESP_FAIL;
    }

    const cJSON *j_pin = cJSON_GetObjectItem(root, "pin");
    const cJSON *j_val = cJSON_GetObjectItem(root, "val");
    const cJSON *j_duty = cJSON_GetObjectItem(root, "duty");

    if (!cJSON_IsNumber(j_pin)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing numeric 'pin' field");
        return ESP_FAIL;
    }

    int pin = j_pin->valueint;
    int duty = cJSON_IsNumber(j_duty) ? j_duty->valueint : -1;
    int val = cJSON_IsNumber(j_val) ? j_val->valueint : -1;
    cJSON_Delete(root);

    esp_err_t op_err = ESP_OK;
    if (duty >= 0) {
        op_err = hal_gpio_set_pwm(pin, (uint8_t)duty);
    } else if (val >= 0) {
        op_err = hal_gpio_write(pin, val);
    }

    if (op_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(op_err));
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"pin\":%d,\"val\":%d,\"duty\":%d}",
             pin, hal_gpio_read(pin), duty >= 0 ? duty : 0);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t gpio_config_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;

    char content[512];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    content[recv_len] = '\0';

    esp_err_t err = hal_gpio_apply_config_json(content, true);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Expected JSON array of pin overrides\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"GPIO configuration saved to NVS\"}");
}

esp_err_t mod_gpio_init(void)
{
    return hal_gpio_init();
}

esp_err_t mod_gpio_register_routes(httpd_handle_t server)
{
    httpd_uri_t uri_pins = {
        .uri       = "/api/gpio/pins",
        .method    = HTTP_GET,
        .handler   = gpio_pins_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_pins);

    httpd_uri_t uri_set = {
        .uri       = "/api/gpio/set",
        .method    = HTTP_POST,
        .handler   = gpio_set_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_set);

    httpd_uri_t uri_cfg = {
        .uri       = "/api/gpio/config",
        .method    = HTTP_POST,
        .handler   = gpio_config_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &uri_cfg);

    ESP_LOGI(TAG, "GPIO REST routes registered (/api/gpio/pins, /api/gpio/set, /api/gpio/config)");
    return ESP_OK;
}
