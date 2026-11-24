#include "hal_gpio.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "hal_gpio";

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT // 0-255
#define LEDC_FREQUENCY          5000             // 5 kHz

// Exposed pins definition
static gpio_pin_desc_t s_pins[] = {
    { .pin_num = 33, .label = "LED_STATUS",   .role = GPIO_PIN_ROLE_LED_IND,   .is_output = true,  .supports_pwm = false, .current_val = 1, .current_duty = 0 },
    { .pin_num = 4,  .label = "FLASH_PWM",    .role = GPIO_PIN_ROLE_FLASH_PWM, .is_output = true,  .supports_pwm = true,  .current_val = 0, .current_duty = 0 },
    { .pin_num = 12, .label = "GPIO_12",      .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true,  .supports_pwm = false, .current_val = 0, .current_duty = 0 },
    { .pin_num = 13, .label = "GPIO_13",      .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true,  .supports_pwm = false, .current_val = 0, .current_duty = 0 },
    { .pin_num = 14, .label = "GPIO_14",      .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true,  .supports_pwm = false, .current_val = 0, .current_duty = 0 },
    { .pin_num = 15, .label = "GPIO_15",      .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true,  .supports_pwm = false, .current_val = 0, .current_duty = 0 },
};
#define NUM_PINS (sizeof(s_pins) / sizeof(s_pins[0]))

static bool s_ledc_init_done = false;

static void ensure_ledc_init(void)
{
    if (s_ledc_init_done) return;
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_DUTY_RES,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQUENCY,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);
    s_ledc_init_done = true;
}

esp_err_t hal_gpio_init(void)
{
    ESP_LOGI(TAG, "Initializing HAL GPIO subsystem (%d registered pins)...", NUM_PINS);

    for (size_t i = 0; i < NUM_PINS; i++) {
        gpio_pin_desc_t *p = &s_pins[i];

        if (p->supports_pwm) {
            ensure_ledc_init();
            ledc_channel_config_t ledc_channel = {
                .speed_mode     = LEDC_MODE,
                .channel        = LEDC_CHANNEL_0,
                .timer_sel      = LEDC_TIMER,
                .intr_type      = LEDC_INTR_DISABLE,
                .gpio_num       = p->pin_num,
                .duty           = 0,
                .hpoint         = 0
            };
            ledc_channel_config(&ledc_channel);
        } else {
            gpio_config_t io_conf = {
                .pin_bit_mask = (1ULL << p->pin_num),
                .mode = p->is_output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&io_conf);
            // Default level: pin 33 is active low (1 is off)
            if (p->pin_num == 33) {
                gpio_set_level(33, 1);
            }
        }
    }
    return ESP_OK;
}

static gpio_pin_desc_t *find_pin(int pin)
{
    for (size_t i = 0; i < NUM_PINS; i++) {
        if (s_pins[i].pin_num == pin) return &s_pins[i];
    }
    return NULL;
}

esp_err_t hal_gpio_write(int pin, int level)
{
    gpio_pin_desc_t *p = find_pin(pin);
    if (!p) return ESP_ERR_NOT_FOUND;

    if (p->supports_pwm) {
        // Map 0 to 0% duty, 1 to 10% duty (safe brightness)
        return hal_gpio_set_pwm(pin, level ? 10 : 0);
    }

    esp_err_t err = gpio_set_level(pin, level ? 1 : 0);
    if (err == ESP_OK) {
        p->current_val = level ? 1 : 0;
    }
    return err;
}

int hal_gpio_read(int pin)
{
    gpio_pin_desc_t *p = find_pin(pin);
    if (!p) return -1;
    if (p->supports_pwm) {
        return (p->current_duty > 0) ? 1 : 0;
    }
    p->current_val = gpio_get_level(pin);
    return p->current_val;
}

esp_err_t hal_gpio_set_pwm(int pin, uint8_t duty_percent)
{
    gpio_pin_desc_t *p = find_pin(pin);
    if (!p || !p->supports_pwm) return ESP_ERR_INVALID_ARG;

    // Safety clamp: Flashlight on pin 4 must not exceed 25% duty (~70mA max) on USB power
    if (pin == 4 && duty_percent > 25) {
        ESP_LOGW(TAG, "Duty %d%% clamped to 25%% for safety against voltage drop", duty_percent);
        duty_percent = 25;
    }

    uint32_t duty_val = (duty_percent * 255) / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, duty_val);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);

    p->current_duty = duty_percent;
    p->current_val = (duty_percent > 0) ? 1 : 0;
    return ESP_OK;
}

size_t hal_gpio_pins_to_json(char *dst, size_t max_len)
{
    if (!dst || max_len < 32) return 0;

    size_t offset = 0;
    offset += snprintf(dst + offset, max_len - offset, "{\"ok\":true,\"pins\":[");

    for (size_t i = 0; i < NUM_PINS; i++) {
        if (offset >= max_len - 128) break;
        gpio_pin_desc_t *p = &s_pins[i];
        if (!p->supports_pwm) {
            p->current_val = gpio_get_level(p->pin_num);
        }

        offset += snprintf(dst + offset, max_len - offset,
            "%s{\"pin\":%d,\"label\":\"%s\",\"role\":%d,\"output\":%s,\"pwm\":%s,\"val\":%d,\"duty\":%d}",
            (i == 0) ? "" : ",",
            p->pin_num, p->label, (int)p->role,
            p->is_output ? "true" : "false",
            p->supports_pwm ? "true" : "false",
            p->current_val,
            p->current_duty);
    }

    offset += snprintf(dst + offset, max_len - offset, "]}");
    return offset;
}
