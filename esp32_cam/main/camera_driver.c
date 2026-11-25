#include "camera_driver.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cam_drv";

esp_err_t camera_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing OV2640 camera (AI-Thinker pinout)...");

    /* Initialize Flashlight LED (GPIO 4) to OFF */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FLASH_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(FLASH_LED_GPIO, 0);

    /* Hardware reset / power-cycle camera module to ensure clean power-on state */
    gpio_set_direction(PWDN_GPIO_NUM, GPIO_MODE_OUTPUT);
    gpio_set_level(PWDN_GPIO_NUM, 1); // Power down
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PWDN_GPIO_NUM, 0); // Power up
    vTaskDelay(pdMS_TO_TICKS(50));

    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,

        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA, // Default 640x480 for smooth 20+ FPS MJPEG
        .jpeg_quality = 12,          // 0-63, lower number means higher quality
        .fb_count = 2,               // Double buffering for fast stream
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = ESP_FAIL;
    for (int retry = 0; retry < 3; retry++) {
        err = esp_camera_init(&config);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "esp_camera_init attempt %d failed (0x%x), retrying power cycle...", retry + 1, err);
        esp_camera_deinit();
        gpio_set_level(PWDN_GPIO_NUM, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PWDN_GPIO_NUM, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init all attempts failed: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        // AI-Thinker OV2640 module usually mounts with vertical flip required
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
        ESP_LOGI(TAG, "Camera sensor detected (PID: 0x%02x)", s->id.PID);
    }

    ESP_LOGI(TAG, "Camera initialized successfully!");
    return ESP_OK;
}

#include "driver/ledc.h"

#define FLASH_LEDC_CHANNEL LEDC_CHANNEL_1
#define FLASH_LEDC_TIMER   LEDC_TIMER_1

void camera_driver_set_flash(bool on)
{
    if (on) {
        /* Safe ~10% PWM duty cycle (25/255) to prevent USB voltage drop/overheating */
        ledc_timer_config_t timer_conf = {
            .duty_resolution = LEDC_TIMER_8_BIT,
            .freq_hz = 5000,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = FLASH_LEDC_TIMER,
        };
        ledc_timer_config(&timer_conf);
        ledc_channel_config_t ch_conf = {
            .channel = FLASH_LEDC_CHANNEL,
            .duty = 25,
            .gpio_num = FLASH_LED_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_sel = FLASH_LEDC_TIMER,
        };
        ledc_channel_config(&ch_conf);
    } else {
        ledc_stop(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL, 0);
        gpio_set_direction(FLASH_LED_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(FLASH_LED_GPIO, 0);
    }
}

esp_err_t camera_driver_set_framesize(framesize_t size)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_ERR_INVALID_STATE;
    return s->set_framesize(s, size) == 0 ? ESP_OK : ESP_FAIL;
}

camera_fb_t *camera_driver_get_fb(void)
{
    return esp_camera_fb_get();
}

void camera_driver_return_fb(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}
