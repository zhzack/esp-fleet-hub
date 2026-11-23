#include "power_sw.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_sw";
static uint8_t s_gpio_pin = DEFAULT_POWER_SW_GPIO;
static bool s_inited = false;

esp_err_t power_sw_init(uint8_t gpio_num)
{
    s_gpio_pin = gpio_num;
    if (s_gpio_pin == 0 || s_gpio_pin >= GPIO_NUM_MAX) {
        ESP_LOGI(TAG, "power_sw disabled (pin=%u)", s_gpio_pin);
        s_inited = false;
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_gpio_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err == ESP_OK) {
        s_inited = true;
        ESP_LOGI(TAG, "power_sw initialized on GPIO %u (idle=High-Z)", s_gpio_pin);
    } else {
        ESP_LOGE(TAG, "failed to configure GPIO %u: %d", s_gpio_pin, err);
    }
    return err;
}

esp_err_t power_sw_pulse(int duration_ms)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "power_sw not initialized or disabled");
        return ESP_ERR_INVALID_STATE;
    }

    int pulse_time = duration_ms > 0 ? duration_ms : 500;
    ESP_LOGI(TAG, "pulsing power switch for %d ms on GPIO %u", pulse_time, s_gpio_pin);

    /* Pull LOW: set to output 0 */
    gpio_set_level(s_gpio_pin, 0);
    gpio_set_direction(s_gpio_pin, GPIO_MODE_OUTPUT);

    vTaskDelay(pdMS_TO_TICKS(pulse_time));

    /* Restore to High-Z (safe input mode) */
    gpio_set_direction(s_gpio_pin, GPIO_MODE_INPUT);
    ESP_LOGI(TAG, "power switch released, pin back to High-Z");

    return ESP_OK;
}

uint8_t power_sw_get_pin(void)
{
    return s_gpio_pin;
}
