#include "hal_camera.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_gpio.h"

static const char *TAG = "hal_cam";
static bool s_camera_ready = false;

// AI-Thinker ESP32-CAM Pinout
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27

#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

static camera_config_t camera_config = {
    .pin_pwdn     = CAM_PIN_PWDN,
    .pin_reset    = CAM_PIN_RESET,
    .pin_xclk     = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href  = CAM_PIN_HREF,
    .pin_pclk  = CAM_PIN_PCLK,

    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_1, // Timer 1 so it doesn't conflict with GPIO PWM on Timer 0
    .ledc_channel = LEDC_CHANNEL_1,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_VGA, // 640x480 for smooth streaming & good quality
    .jpeg_quality = 12,            // 0-63, lower is higher quality
    .fb_count     = 2,             // Dual framebuffers for tear-free MJPEG
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_LATEST
};

esp_err_t hal_camera_init(void)
{
    ESP_LOGI(TAG, "Initializing OV2640 / OV5640 Camera with PWDN reset sequence...");

    // Hardware power-cycle sequence on PWDN (GPIO 32)
    gpio_config_t pwdn_conf = {
        .pin_bit_mask = (1ULL << CAM_PIN_PWDN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwdn_conf);

    // Assert PWDN high to power down sensor
    gpio_set_level(CAM_PIN_PWDN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    // Release PWDN low to power up sensor
    gpio_set_level(CAM_PIN_PWDN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Try probe up to 3 times
    esp_err_t err = ESP_FAIL;
    for (int retry = 1; retry <= 3; retry++) {
        ESP_LOGI(TAG, "Camera probe attempt %d/3...", retry);
        err = esp_camera_init(&camera_config);
        if (err == ESP_OK) {
            sensor_t *s = esp_camera_sensor_get();
            if (s) {
                // Adjust default sensor settings for good indoor lighting
                s->set_brightness(s, 1);
                s->set_contrast(s, 0);
                s->set_saturation(s, 0);
                // Flip vertically if mounted inverted
                s->set_vflip(s, 1);
                s->set_hmirror(s, 0);
            }
            s_camera_ready = true;
            ESP_LOGI(TAG, "Camera sensor probed and configured successfully!");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Camera init attempt %d failed (0x%x), deiniting and retrying...", retry, err);
        esp_camera_deinit();
        gpio_set_level(CAM_PIN_PWDN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(CAM_PIN_PWDN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "Camera probe completely failed after 3 attempts: %s", esp_err_to_name(err));
    s_camera_ready = false;
    return err;
}

camera_fb_t *hal_camera_fb_get(void)
{
    if (!s_camera_ready) return NULL;
    return esp_camera_fb_get();
}

void hal_camera_fb_return(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

esp_err_t hal_camera_set_flash_duty(uint8_t duty_percent)
{
    return hal_gpio_set_pwm(4, duty_percent);
}

uint8_t hal_camera_get_flash_duty(void)
{
    // GPIO 4 duty
    return 0; // tracked in hal_gpio
}

bool hal_camera_is_ready(void)
{
    return s_camera_ready;
}
