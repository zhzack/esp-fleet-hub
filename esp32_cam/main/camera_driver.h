#pragma once

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard AI-Thinker ESP32-CAM Pinout */
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

/* Onboard High-Power Flashlight LED */
#define FLASH_LED_GPIO     4

/**
 * Initialize OV2640 camera with AI-Thinker pinout and PSRAM frame buffer.
 */
esp_err_t camera_driver_init(void);

/**
 * Turn flashlight on/off.
 */
void camera_driver_set_flash(bool on);

/**
 * Set camera framesize (e.g. FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_HD, FRAMESIZE_UXGA).
 */
esp_err_t camera_driver_set_framesize(framesize_t size);

/**
 * Capture a single JPEG frame.
 * Caller must call camera_driver_return_fb() after processing.
 */
camera_fb_t *camera_driver_get_fb(void);

/**
 * Return frame buffer to camera driver.
 */
void camera_driver_return_fb(camera_fb_t *fb);

#ifdef __cplusplus
}
#endif
