#include "mod_gpio.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "hal_gpio.h"

static const char *TAG = "mod_gpio";

static esp_err_t gpio_pins_get_handler(httpd_req_t *req)
{
    char buf[1024];
    size_t len = hal_gpio_pins_to_json(buf, sizeof(buf));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t gpio_set_post_handler(httpd_req_t *req)
{
    char content[256];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing payload");
        return ESP_FAIL;
    }
    content[recv_len] = '\0';

    // Parse simple JSON: {"pin": X, "val": Y, "duty": Z}
    int pin = -1;
    int val = -1;
    int duty = -1;

    char *p_pin = strstr(content, "\"pin\":");
    if (p_pin) pin = atoi(p_pin + 6);

    char *p_val = strstr(content, "\"val\":");
    if (p_val) val = atoi(p_val + 6);

    char *p_duty = strstr(content, "\"duty\":");
    if (p_duty) duty = atoi(p_duty + 7);

    if (pin < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'pin' field");
        return ESP_FAIL;
    }

    if (duty >= 0) {
        hal_gpio_set_pwm(pin, (uint8_t)duty);
    } else if (val >= 0) {
        hal_gpio_write(pin, val);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"pin\":%d,\"val\":%d,\"duty\":%d}",
             pin, hal_gpio_read(pin), duty >= 0 ? duty : 0);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
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

    ESP_LOGI(TAG, "GPIO REST routes registered (/api/gpio/pins, /api/gpio/set)");
    return ESP_OK;
}
