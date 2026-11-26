#include "board_pins.h"
#include "sdkconfig.h"
#include <string.h>

static bool is_pin_reserved(int pin)
{
#if CONFIG_FEATURE_PC_MONITOR
    if (pin == BOARD_ST7735_MOSI || pin == BOARD_ST7735_CLK ||
        pin == BOARD_ST7735_CS   || pin == BOARD_ST7735_DC  ||
        pin == BOARD_ST7735_RST  || pin == BOARD_ST7735_BCKL) {
        return true;
    }
#endif

#if CONFIG_FEATURE_JOYSTICK
    if (pin == BOARD_JOY_UP || pin == BOARD_JOY_DOWN ||
        pin == BOARD_JOY_LEFT || pin == BOARD_JOY_RIGHT ||
        pin == BOARD_JOY_CENTER) {
        return true;
    }
#endif

#if CONFIG_FEATURE_DISPLAY && !CONFIG_FEATURE_PC_MONITOR
    if (pin >= 0 && (pin == BOARD_DISP_MOSI || pin == BOARD_DISP_CLK ||
        pin == BOARD_DISP_CS || pin == BOARD_DISP_DC ||
        pin == BOARD_DISP_RST || pin == BOARD_DISP_BUSY ||
        pin == BOARD_DISP_BCKL)) {
        return true;
    }
#endif

#if CONFIG_FEATURE_CAMERA
    int cam_pins[] = {
        BOARD_CAM_PIN_PWDN, BOARD_CAM_PIN_RESET, BOARD_CAM_PIN_XCLK,
        BOARD_CAM_PIN_SIOD, BOARD_CAM_PIN_SIOC,
        BOARD_CAM_PIN_D7, BOARD_CAM_PIN_D6, BOARD_CAM_PIN_D5, BOARD_CAM_PIN_D4,
        BOARD_CAM_PIN_D3, BOARD_CAM_PIN_D2, BOARD_CAM_PIN_D1, BOARD_CAM_PIN_D0,
        BOARD_CAM_PIN_VSYNC, BOARD_CAM_PIN_HREF, BOARD_CAM_PIN_PCLK
    };
    for (size_t i = 0; i < sizeof(cam_pins) / sizeof(cam_pins[0]); i++) {
        if (cam_pins[i] >= 0 && cam_pins[i] == pin) return true;
    }
#endif

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM_MODE_OCT
    if (pin >= 33 && pin <= 37) return true;
#endif

    return false;
}

const char *board_pins_get_name(void)
{
#if CONFIG_BOARD_ESP32_CAM_AITHINKER
    return "AI-Thinker ESP32-CAM";
#elif CONFIG_BOARD_ESP32S3_CAM_DEV
    return "ESP32-S3 CAM DevKit";
#elif CONFIG_BOARD_ESP32S3_DEVKIT
    return "ESP32-S3 DevKitC";
#elif CONFIG_BOARD_ESP32C3_AIR101
    return "LuatOS ESP32-C3 + Air101";
#elif CONFIG_BOARD_ESP32C3_SUPERMINI
    return "ESP32-C3 SuperMini";
#elif CONFIG_BOARD_ESP32_GENERIC
    return "ESP32 DevKit Generic";
#elif CONFIG_BOARD_ESP32C2_GENERIC
    return "ESP32-C2 Minimal";
#else
    // Auto resolved by Target + Active Peripherals
    #if CONFIG_IDF_TARGET_ESP32C3
        #if CONFIG_FEATURE_PC_MONITOR
            return "ESP32-C3 Air101 (Auto)";
        #else
            return "ESP32-C3 Core (Auto)";
        #endif
    #elif CONFIG_IDF_TARGET_ESP32S3
        #if CONFIG_FEATURE_CAMERA
            return "ESP32-S3 CAM (Auto)";
        #else
            return "ESP32-S3 DevKit (Auto)";
        #endif
    #elif CONFIG_IDF_TARGET_ESP32C2
        return "ESP32-C2 Node (Auto)";
    #else
        #if CONFIG_FEATURE_CAMERA
            return "ESP32-CAM AI-Thinker (Auto)";
        #else
            return "ESP32 DevKit (Auto)";
        #endif
    #endif
#endif
}

const char *board_pins_get_slug(void)
{
#if CONFIG_BOARD_ESP32_CAM_AITHINKER
    return "esp32_cam_aithinker";
#elif CONFIG_BOARD_ESP32S3_CAM_DEV
    return "esp32s3_cam";
#elif CONFIG_BOARD_ESP32S3_DEVKIT
    return "esp32s3_devkit";
#elif CONFIG_BOARD_ESP32C3_AIR101
    return "esp32c3_air101";
#elif CONFIG_BOARD_ESP32C3_SUPERMINI
    return "esp32c3_supermini";
#elif CONFIG_BOARD_ESP32_GENERIC
    return "esp32_generic";
#elif CONFIG_BOARD_ESP32C2_GENERIC
    return "esp32c2_generic";
#else
    #if CONFIG_IDF_TARGET_ESP32C3
        #if CONFIG_FEATURE_PC_MONITOR
            return "esp32c3_air101";
        #else
            return "esp32c3_core";
        #endif
    #elif CONFIG_IDF_TARGET_ESP32S3
        #if CONFIG_FEATURE_CAMERA
            return "esp32s3_cam";
        #else
            return "esp32s3_devkit";
        #endif
    #elif CONFIG_IDF_TARGET_ESP32C2
        return "esp32c2_generic";
    #else
        #if CONFIG_FEATURE_CAMERA
            return "esp32_cam_aithinker";
        #else
            return "esp32_generic";
        #endif
    #endif
#endif
}

static const gpio_pin_desc_t *get_raw_board_table(size_t *out_len)
{
#if CONFIG_IDF_TARGET_ESP32C3
    #if CONFIG_BOARD_ESP32C3_SUPERMINI
    static const gpio_pin_desc_t tbl[] = {
        { .pin_num = 8,  .label = "LED_BLUE",   .role = GPIO_PIN_ROLE_LED_IND, .is_output = true, .supports_pwm = false, .current_val = 1, .current_duty = 0, .active_low = true },
        { .pin_num = 0,  .label = "GPIO_0",     .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 1,  .label = "GPIO_1",     .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 2,  .label = "PWM_LED_2",  .role = GPIO_PIN_ROLE_RELAY,   .is_output = true, .supports_pwm = true,  .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 3,  .label = "GPIO_3",     .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 10, .label = "GPIO_10",    .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
    };
    #else
    // ESP32-C3 Core / Air101: D4=12, D5=13 (note 13 filtered automatically if Joystick enabled!)
    static const gpio_pin_desc_t tbl[] = {
        { .pin_num = 12, .label = "LED_D4",     .role = GPIO_PIN_ROLE_LED_IND, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 13, .label = "LED_D5",     .role = GPIO_PIN_ROLE_LED_IND, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 0,  .label = "GPIO_0",     .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 1,  .label = "GPIO_1",     .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 4,  .label = "RELAY_4",    .role = GPIO_PIN_ROLE_RELAY,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 5,  .label = "PWM_5",      .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = true,  .current_val = 0, .current_duty = 0, .active_low = false },
    };
    #endif
#elif CONFIG_IDF_TARGET_ESP32S3
    static const gpio_pin_desc_t tbl[] = {
        { .pin_num = 2,  .label = "GPIO_2",     .role = GPIO_PIN_ROLE_LED_IND, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 1,  .label = "GPIO_1",     .role = GPIO_PIN_ROLE_RELAY,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 4,  .label = "PWM_4",      .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = true,  .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 14, .label = "GPIO_14",    .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 21, .label = "GPIO_21",    .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 47, .label = "GPIO_47",    .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
    };
#elif CONFIG_IDF_TARGET_ESP32C2
    static const gpio_pin_desc_t tbl[] = {
        { .pin_num = 0,  .label = "GPIO_0",     .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 1,  .label = "GPIO_1",     .role = GPIO_PIN_ROLE_GENERIC, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 4,  .label = "PWM_4",      .role = GPIO_PIN_ROLE_RELAY,   .is_output = true, .supports_pwm = true,  .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 18, .label = "LED_18",     .role = GPIO_PIN_ROLE_LED_IND, .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
    };
#else
    // Classic ESP32 (CAM or Generic DevKit)
    #if CONFIG_BOARD_ESP32_CAM_AITHINKER || (CONFIG_BOARD_AUTO_DEFAULT && CONFIG_FEATURE_CAMERA)
    static const gpio_pin_desc_t tbl[] = {
        { .pin_num = 33, .label = "LED_STATUS", .role = GPIO_PIN_ROLE_LED_IND,   .is_output = true, .supports_pwm = false, .current_val = 1, .current_duty = 0, .active_low = true },
        { .pin_num = 4,  .label = "FLASH_PWM",  .role = GPIO_PIN_ROLE_FLASH_PWM, .is_output = true, .supports_pwm = true,  .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 12, .label = "GPIO_12",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 13, .label = "GPIO_13",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 14, .label = "GPIO_14",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 15, .label = "GPIO_15",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
    };
    #else
    static const gpio_pin_desc_t tbl[] = {
        { .pin_num = 2,  .label = "LED_BUILTIN",.role = GPIO_PIN_ROLE_LED_IND,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 4,  .label = "PWM_4",      .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = true,  .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 12, .label = "GPIO_12",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 13, .label = "GPIO_13",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 14, .label = "GPIO_14",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
        { .pin_num = 15, .label = "GPIO_15",    .role = GPIO_PIN_ROLE_GENERIC,   .is_output = true, .supports_pwm = false, .current_val = 0, .current_duty = 0, .active_low = false },
    };
    #endif
#endif
    *out_len = sizeof(tbl) / sizeof(tbl[0]);
    return tbl;
}

size_t board_pins_populate_gpio(gpio_pin_desc_t *dst, size_t max_items)
{
    if (!dst || max_items == 0) return 0;

    size_t raw_count = 0;
    const gpio_pin_desc_t *raw = get_raw_board_table(&raw_count);
    size_t added = 0;

    for (size_t i = 0; i < raw_count && added < max_items; i++) {
        // Keep FLASH_PWM on AI-Thinker Camera even when camera is enabled
        bool allow_cam_flash = (raw[i].role == GPIO_PIN_ROLE_FLASH_PWM);
        if (!allow_cam_flash && is_pin_reserved(raw[i].pin_num)) {
            continue;
        }
        memcpy(&dst[added], &raw[i], sizeof(gpio_pin_desc_t));
        added++;
    }
    return added;
}
