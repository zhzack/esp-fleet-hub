#include "hal_camera.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "hal_gpio.h"
#include "board_pins.h"

static const char *TAG = "hal_cam";
static bool s_camera_ready = false;

#if CONFIG_FEATURE_CAMERA

static camera_config_t camera_config = {
    .pin_pwdn     = BOARD_CAM_PIN_PWDN,
    .pin_reset    = BOARD_CAM_PIN_RESET,
    .pin_xclk     = BOARD_CAM_PIN_XCLK,
    .pin_sccb_sda = BOARD_CAM_PIN_SIOD,
    .pin_sccb_scl = BOARD_CAM_PIN_SIOC,

    .pin_d7 = BOARD_CAM_PIN_D7,
    .pin_d6 = BOARD_CAM_PIN_D6,
    .pin_d5 = BOARD_CAM_PIN_D5,
    .pin_d4 = BOARD_CAM_PIN_D4,
    .pin_d3 = BOARD_CAM_PIN_D3,
    .pin_d2 = BOARD_CAM_PIN_D2,
    .pin_d1 = BOARD_CAM_PIN_D1,
    .pin_d0 = BOARD_CAM_PIN_D0,
    .pin_vsync = BOARD_CAM_PIN_VSYNC,
    .pin_href  = BOARD_CAM_PIN_HREF,
    .pin_pclk  = BOARD_CAM_PIN_PCLK,

    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_1, // Timer 1 so it doesn't conflict with GPIO PWM on Timer 0
    .ledc_channel = LEDC_CHANNEL_1,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_VGA, // 640x480 for smooth streaming & good quality
    .jpeg_quality = 12,            // 0-63, lower is higher quality
    .fb_count     = 2,             // Dual framebuffers for tear-free MJPEG
#if CONFIG_SPIRAM
    .fb_location  = CAMERA_FB_IN_PSRAM,
#else
    .fb_location  = CAMERA_FB_IN_DRAM,
#endif
    .grab_mode    = CAMERA_GRAB_LATEST
};

static void camera_power_cycle(int delay_ms)
{
    if (BOARD_CAM_PIN_PWDN < 0) return;
    gpio_set_level(BOARD_CAM_PIN_PWDN, 1);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    gpio_set_level(BOARD_CAM_PIN_PWDN, 0);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

esp_err_t hal_camera_init(void)
{
    ESP_LOGI(TAG, "Initializing OV2640 / OV5640 Camera (%s)...", board_pins_get_name());

    if (BOARD_CAM_PIN_PWDN >= 0) {
        gpio_config_t pwdn_conf = {
            .pin_bit_mask = (1ULL << BOARD_CAM_PIN_PWDN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&pwdn_conf);
        camera_power_cycle(50);
    }

    app_config_t cfg;
    app_config_get(&cfg);
    if (cfg.cam_quality >= 4 && cfg.cam_quality <= 63) {
        camera_config.jpeg_quality = cfg.cam_quality;
    }
    if (cfg.cam_framesize <= FRAMESIZE_UXGA) {
        camera_config.frame_size = (framesize_t)cfg.cam_framesize;
    }

    // Try probe up to 3 times
    esp_err_t err = ESP_FAIL;
    for (int retry = 1; retry <= 3; retry++) {
        ESP_LOGI(TAG, "Camera probe attempt %d/3...", retry);
        err = esp_camera_init(&camera_config);
        if (err == ESP_OK) {
            sensor_t *s = esp_camera_sensor_get();
            if (s) {
                s->set_brightness(s, cfg.cam_brightness);
                s->set_contrast(s, 0);
                s->set_saturation(s, 0);
                s->set_vflip(s, cfg.cam_vflip ? 1 : 0);
                s->set_hmirror(s, cfg.cam_hmirror ? 1 : 0);
            }
            s_camera_ready = true;
            ESP_LOGI(TAG, "Camera sensor probed (fs=%u q=%u vflip=%d)!",
                     cfg.cam_framesize, cfg.cam_quality, cfg.cam_vflip);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Camera init attempt %d failed (0x%x), deiniting and retrying...", retry, err);
        esp_camera_deinit();
        camera_power_cycle(100);
    }

    ESP_LOGE(TAG, "Camera probe completely failed after 3 attempts: %s", esp_err_to_name(err));
    s_camera_ready = false;
    return err;
}

esp_err_t hal_camera_apply_config(uint8_t framesize, uint8_t quality, bool vflip, bool hmirror, int8_t brightness)
{
    if (!s_camera_ready) return ESP_ERR_INVALID_STATE;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_FAIL;

    if (framesize <= FRAMESIZE_UXGA && s->set_framesize) {
        s->set_framesize(s, (framesize_t)framesize);
    }
    if (quality >= 4 && quality <= 63 && s->set_quality) {
        s->set_quality(s, quality);
    }
    if (s->set_vflip) s->set_vflip(s, vflip ? 1 : 0);
    if (s->set_hmirror) s->set_hmirror(s, hmirror ? 1 : 0);
    if (s->set_brightness) s->set_brightness(s, brightness);

    ESP_LOGI(TAG, "Camera live config updated: fs=%u q=%u vflip=%d hmirror=%d bri=%d",
             framesize, quality, vflip, hmirror, brightness);
    return ESP_OK;
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
    return hal_gpio_set_pwm(BOARD_CAM_FLASH_GPIO, duty_percent);
}

uint8_t hal_camera_get_flash_duty(void)
{
    return 0;
}

bool hal_camera_is_ready(void)
{
    return s_camera_ready;
}

#else

esp_err_t hal_camera_init(void) { return ESP_ERR_NOT_SUPPORTED; }
camera_fb_t *hal_camera_fb_get(void) { return NULL; }
void hal_camera_fb_return(camera_fb_t *fb) { (void)fb; }
esp_err_t hal_camera_set_flash_duty(uint8_t duty_percent) { (void)duty_percent; return ESP_ERR_NOT_SUPPORTED; }
uint8_t hal_camera_get_flash_duty(void) { return 0; }
esp_err_t hal_camera_apply_config(uint8_t framesize, uint8_t quality, bool vflip, bool hmirror, int8_t brightness) {
    (void)framesize; (void)quality; (void)vflip; (void)hmirror; (void)brightness;
    return ESP_ERR_NOT_SUPPORTED;
}
bool hal_camera_is_ready(void) { return false; }

#endif
