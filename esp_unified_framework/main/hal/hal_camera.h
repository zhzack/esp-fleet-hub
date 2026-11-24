#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hal_camera_init(void);

camera_fb_t *hal_camera_fb_get(void);
void hal_camera_fb_return(camera_fb_t *fb);

esp_err_t hal_camera_set_flash_duty(uint8_t duty_percent);
uint8_t hal_camera_get_flash_duty(void);

bool hal_camera_is_ready(void);

#ifdef __cplusplus
}
#endif
