#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "hal_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get human-readable board profile name (resolved from Kconfig + Target)
 */
const char *board_pins_get_name(void);

/**
 * @brief Get resolved board profile slug identifier
 */
const char *board_pins_get_slug(void);

/**
 * @brief Copy board-safe GPIO table into dst array (filtering pins reserved by active peripherals)
 * @param dst Destination array of gpio_pin_desc_t
 * @param max_items Capacity of dst array
 * @return Actual number of pins populated
 */
size_t board_pins_populate_gpio(gpio_pin_desc_t *dst, size_t max_items);

/* ================= Camera Pinout Mapping ================= */
#if CONFIG_BOARD_ESP32S3_CAM_DEV || (CONFIG_BOARD_AUTO_DEFAULT && CONFIG_IDF_TARGET_ESP32S3)
    // Common ESP32-S3-CAM (Freenove / Goouuu S3-EYE compatible)
    #define BOARD_CAM_PIN_PWDN    -1
    #define BOARD_CAM_PIN_RESET   -1
    #define BOARD_CAM_PIN_XCLK    15
    #define BOARD_CAM_PIN_SIOD     4
    #define BOARD_CAM_PIN_SIOC     5
    #define BOARD_CAM_PIN_D7      16
    #define BOARD_CAM_PIN_D6      17
    #define BOARD_CAM_PIN_D5      18
    #define BOARD_CAM_PIN_D4      12
    #define BOARD_CAM_PIN_D3      10
    #define BOARD_CAM_PIN_D2       8
    #define BOARD_CAM_PIN_D1       9
    #define BOARD_CAM_PIN_D0      11
    #define BOARD_CAM_PIN_VSYNC    6
    #define BOARD_CAM_PIN_HREF     7
    #define BOARD_CAM_PIN_PCLK    13
    #define BOARD_CAM_FLASH_GPIO  48
#else
    // Default: AI-Thinker ESP32-CAM
    #define BOARD_CAM_PIN_PWDN    32
    #define BOARD_CAM_PIN_RESET   -1
    #define BOARD_CAM_PIN_XCLK     0
    #define BOARD_CAM_PIN_SIOD    26
    #define BOARD_CAM_PIN_SIOC    27
    #define BOARD_CAM_PIN_D7      35
    #define BOARD_CAM_PIN_D6      34
    #define BOARD_CAM_PIN_D5      39
    #define BOARD_CAM_PIN_D4      36
    #define BOARD_CAM_PIN_D3      21
    #define BOARD_CAM_PIN_D2      19
    #define BOARD_CAM_PIN_D1      18
    #define BOARD_CAM_PIN_D0       5
    #define BOARD_CAM_PIN_VSYNC   25
    #define BOARD_CAM_PIN_HREF    23
    #define BOARD_CAM_PIN_PCLK    22
    #define BOARD_CAM_FLASH_GPIO   4
#endif

/* ================= Air101 ST7735 & Joystick Mapping (ESP32-C3) ================= */
#define BOARD_ST7735_MOSI      3
#define BOARD_ST7735_CLK       2
#define BOARD_ST7735_CS        7
#define BOARD_ST7735_DC        6
#define BOARD_ST7735_RST      10
#define BOARD_ST7735_BCKL     11

#define BOARD_JOY_UP           9
#define BOARD_JOY_DOWN         5
#define BOARD_JOY_LEFT         8
#define BOARD_JOY_RIGHT       13
#define BOARD_JOY_CENTER       4

/* ================= Generic SPI Display Mapping (E-Paper SSD1683 / ST7789 LCD) ================= */
#if CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_FEATURE_CAMERA
    /* Aligned 1-to-1 sequentially with Driver Board v1.4 (SCK,SDA,RST,DC,CS1,BUSY -> GPIO 9..14) */
    #define BOARD_DISP_CLK     9
    #define BOARD_DISP_MOSI   10
    #define BOARD_DISP_RST    11
    #define BOARD_DISP_DC     12
    #define BOARD_DISP_CS     13
    #define BOARD_DISP_BUSY   14
    #define BOARD_DISP_BCKL   48
#elif CONFIG_IDF_TARGET_ESP32 && !CONFIG_FEATURE_CAMERA
    #define BOARD_DISP_MOSI   23
    #define BOARD_DISP_CLK    18
    #define BOARD_DISP_CS      5
    #define BOARD_DISP_DC     16
    #define BOARD_DISP_RST    17
    #define BOARD_DISP_BUSY   -1
    #define BOARD_DISP_BCKL   19
#else
    #define BOARD_DISP_MOSI   -1
    #define BOARD_DISP_CLK    -1
    #define BOARD_DISP_CS     -1
    #define BOARD_DISP_DC     -1
    #define BOARD_DISP_RST    -1
    #define BOARD_DISP_BUSY   -1
    #define BOARD_DISP_BCKL   -1
#endif

#ifdef __cplusplus
}
#endif
