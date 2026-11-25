#include "hal_gpio.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "driver/ledc.h"
#include "cJSON.h"
#include "board_pins.h"
#include "app_config.h"

static const char *TAG = "hal_gpio";

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT // 0-255
#define LEDC_FREQUENCY          5000             // 5 kHz
#define MAX_MANAGED_PINS        12

static gpio_pin_desc_t s_pins[MAX_MANAGED_PINS];
static size_t s_num_pins = 0;
static bool s_ledc_init_done = false;
static uint8_t s_next_ledc_ch = 0;
static int s_pwm_channel_map[MAX_MANAGED_PINS];

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
    s_num_pins = board_pins_populate_gpio(s_pins, MAX_MANAGED_PINS);
    s_next_ledc_ch = 0;

    ESP_LOGI(TAG, "Initializing HAL GPIO for board [%s] (%d safe pins)...",
             board_pins_get_name(), (int)s_num_pins);

    for (size_t i = 0; i < s_num_pins; i++) {
        gpio_pin_desc_t *p = &s_pins[i];
        s_pwm_channel_map[i] = -1;

        if (!GPIO_IS_VALID_GPIO(p->pin_num)) {
            ESP_LOGW(TAG, "Skipping invalid GPIO %d on current SoC", p->pin_num);
            continue;
        }

        if (p->supports_pwm && s_next_ledc_ch < 4) {
            ensure_ledc_init();
            ledc_channel_t ch = (ledc_channel_t)(LEDC_CHANNEL_0 + s_next_ledc_ch);
            ledc_channel_config_t ledc_channel = {
                .speed_mode     = LEDC_MODE,
                .channel        = ch,
                .timer_sel      = LEDC_TIMER,
                .intr_type      = LEDC_INTR_DISABLE,
                .gpio_num       = p->pin_num,
                .duty           = 0,
                .hpoint         = 0
            };
            if (ledc_channel_config(&ledc_channel) == ESP_OK) {
                s_pwm_channel_map[i] = (int)ch;
                s_next_ledc_ch++;
            }
        } else {
            gpio_config_t io_conf = {
                .pin_bit_mask = (1ULL << p->pin_num),
                .mode = p->is_output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&io_conf);
            if (p->is_output) {
                gpio_set_level(p->pin_num, p->current_val ? 1 : 0);
            }
        }
    }

    // Apply NVS saved pin labels & default levels if present
    app_config_t cfg;
    app_config_get(&cfg);
    if (cfg.gpio_cfg_json[0] == '[') {
        hal_gpio_apply_config_json(cfg.gpio_cfg_json, false);
    }

    return ESP_OK;
}

static int find_pin_index(int pin)
{
    for (size_t i = 0; i < s_num_pins; i++) {
        if (s_pins[i].pin_num == pin) return (int)i;
    }
    return -1;
}

esp_err_t hal_gpio_write(int pin, int level)
{
    int idx = find_pin_index(pin);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    gpio_pin_desc_t *p = &s_pins[idx];

    if (p->supports_pwm && s_pwm_channel_map[idx] >= 0) {
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
    int idx = find_pin_index(pin);
    if (idx < 0) return -1;
    gpio_pin_desc_t *p = &s_pins[idx];
    if (p->supports_pwm && s_pwm_channel_map[idx] >= 0) {
        return (p->current_duty > 0) ? 1 : 0;
    }
    p->current_val = gpio_get_level(pin);
    return p->current_val;
}

esp_err_t hal_gpio_set_pwm(int pin, uint8_t duty_percent)
{
    int idx = find_pin_index(pin);
    if (idx < 0 || !s_pins[idx].supports_pwm || s_pwm_channel_map[idx] < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_pin_desc_t *p = &s_pins[idx];

    // Safety clamp on Camera Flash LED to avoid USB brownout
    if (p->role == GPIO_PIN_ROLE_FLASH_PWM && duty_percent > 25) {
        ESP_LOGW(TAG, "Flash PWM %d%% clamped to 25%% for power safety", duty_percent);
        duty_percent = 25;
    } else if (duty_percent > 100) {
        duty_percent = 100;
    }

    uint32_t duty_val = (duty_percent * 255) / 100;
    ledc_channel_t ch = (ledc_channel_t)s_pwm_channel_map[idx];
    ledc_set_duty(LEDC_MODE, ch, duty_val);
    ledc_update_duty(LEDC_MODE, ch);

    p->current_duty = duty_percent;
    p->current_val = (duty_percent > 0) ? 1 : 0;
    return ESP_OK;
}

size_t hal_gpio_get_count(void)
{
    return s_num_pins;
}

const gpio_pin_desc_t *hal_gpio_get_pins(size_t *out_count)
{
    if (out_count) *out_count = s_num_pins;
    return s_pins;
}

esp_err_t hal_gpio_set_label(int pin, const char *label)
{
    if (!label) return ESP_ERR_INVALID_ARG;
    int idx = find_pin_index(pin);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    strlcpy(s_pins[idx].label, label, sizeof(s_pins[idx].label));
    return ESP_OK;
}

esp_err_t hal_gpio_apply_config_json(const char *json_str, bool persist)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    cJSON *arr = cJSON_Parse(json_str);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) continue;
        const cJSON *j_pin = cJSON_GetObjectItem(item, "pin");
        if (!cJSON_IsNumber(j_pin)) continue;
        int idx = find_pin_index(j_pin->valueint);
        if (idx < 0) continue;

        const cJSON *j_lbl = cJSON_GetObjectItem(item, "label");
        if (cJSON_IsString(j_lbl) && j_lbl->valuestring[0]) {
            strlcpy(s_pins[idx].label, j_lbl->valuestring, sizeof(s_pins[idx].label));
        }
        const cJSON *j_def = cJSON_GetObjectItem(item, "default_val");
        if (cJSON_IsNumber(j_def) && s_pins[idx].is_output) {
            hal_gpio_write(s_pins[idx].pin_num, j_def->valueint ? 1 : 0);
        }
    }
    cJSON_Delete(arr);

    if (persist) {
        // Re-serialize normalized overrides (label + current state) to NVS
        cJSON *save_arr = cJSON_CreateArray();
        for (size_t i = 0; i < s_num_pins; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "pin", s_pins[i].pin_num);
            cJSON_AddStringToObject(o, "label", s_pins[i].label);
            cJSON_AddNumberToObject(o, "default_val", s_pins[i].current_val);
            cJSON_AddItemToArray(save_arr, o);
        }
        char *printed = cJSON_PrintUnformatted(save_arr);
        cJSON_Delete(save_arr);
        if (printed) {
            app_config_save_gpio_json(printed);
            cJSON_free(printed);
        }
    }
    return ESP_OK;
}

size_t hal_gpio_pins_to_json(char *dst, size_t max_len)
{
    if (!dst || max_len < 32) return 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "board", board_pins_get_name());
    cJSON *arr = cJSON_AddArrayToObject(root, "pins");

    for (size_t i = 0; i < s_num_pins; i++) {
        gpio_pin_desc_t *p = &s_pins[i];
        if (!(p->supports_pwm && s_pwm_channel_map[i] >= 0)) {
            p->current_val = gpio_get_level(p->pin_num);
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "pin", p->pin_num);
        cJSON_AddStringToObject(item, "label", p->label);
        cJSON_AddNumberToObject(item, "role", (int)p->role);
        cJSON_AddBoolToObject(item, "output", p->is_output);
        cJSON_AddBoolToObject(item, "pwm", p->supports_pwm);
        cJSON_AddBoolToObject(item, "active_low", p->active_low);
        cJSON_AddNumberToObject(item, "val", p->current_val);
        cJSON_AddNumberToObject(item, "duty", p->current_duty);
        cJSON_AddItemToArray(arr, item);
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        dst[0] = '\0';
        return 0;
    }
    size_t len = strlcpy(dst, printed, max_len);
    cJSON_free(printed);
    return (len < max_len) ? len : (max_len - 1);
}
