#include "hal_joystick.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "JOYSTICK"

// Portrait 80x160 orientation key mapping:
#define PIN_KEY_UP     BOARD_JOY_UP     // Push UP towards screen header (Vol +)
#define PIN_KEY_DOWN   BOARD_JOY_DOWN   // Push DOWN towards screen footer (Vol -)
#define PIN_KEY_LEFT   BOARD_JOY_LEFT   // Push LEFT towards screen left (Prev)
#define PIN_KEY_RIGHT  BOARD_JOY_RIGHT  // Push RIGHT towards screen right (Next)
#define PIN_KEY_CENTER BOARD_JOY_CENTER // Center Press (Play / Pause)

static joystick_callback_t s_key_cb = NULL;

static void joystick_task(void *pvParameters) {
    const gpio_num_t pins[] = { PIN_KEY_UP, PIN_KEY_DOWN, PIN_KEY_LEFT, PIN_KEY_RIGHT, PIN_KEY_CENTER };
    const joystick_key_t keys[] = { JOYSTICK_UP, JOYSTICK_DOWN, JOYSTICK_LEFT, JOYSTICK_RIGHT, JOYSTICK_CENTER };
    const int num_keys = 5;

    int press_counts[5] = {0, 0, 0, 0, 0};

    while (1) {
        for (int i = 0; i < num_keys; i++) {
            int level = gpio_get_level(pins[i]);
            if (level == 0) { // Active LOW
                press_counts[i]++;
                // Initial trigger after 2 ticks (40ms debounce)
                if (press_counts[i] == 2) {
                    if (s_key_cb) s_key_cb(keys[i]);
                } 
                // Hold repeat for UP and DOWN (volume continuous adjustment)
                else if (press_counts[i] > 18 && (keys[i] == JOYSTICK_UP || keys[i] == JOYSTICK_DOWN)) {
                    if ((press_counts[i] % 5) == 0) { // Repeat every ~100ms
                        if (s_key_cb) s_key_cb(keys[i]);
                    }
                }
            } else {
                press_counts[i] = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t joystick_init(joystick_callback_t cb) {
    s_key_cb = cb;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_KEY_UP) |
                        (1ULL << PIN_KEY_DOWN) |
                        (1ULL << PIN_KEY_LEFT) |
                        (1ULL << PIN_KEY_RIGHT) |
                        (1ULL << PIN_KEY_CENTER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    xTaskCreate(joystick_task, "joystick_task", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Joystick initialized with debouncing");
    return ESP_OK;
}
