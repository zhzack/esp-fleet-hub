#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GPIO_PIN_ROLE_GENERIC = 0,
    GPIO_PIN_ROLE_LED_IND,
    GPIO_PIN_ROLE_FLASH_PWM,
    GPIO_PIN_ROLE_BUTTON,
    GPIO_PIN_ROLE_RELAY,
} gpio_pin_role_t;

typedef struct {
    int pin_num;
    const char *label;
    gpio_pin_role_t role;
    bool is_output;
    bool supports_pwm;
    int current_val;     // 0 or 1
    uint8_t current_duty;// 0 - 100%
} gpio_pin_desc_t;

esp_err_t hal_gpio_init(void);

esp_err_t hal_gpio_write(int pin, int level);
int hal_gpio_read(int pin);
esp_err_t hal_gpio_set_pwm(int pin, uint8_t duty_percent);

size_t hal_gpio_pins_to_json(char *dst, size_t max_len);

#ifdef __cplusplus
}
#endif
