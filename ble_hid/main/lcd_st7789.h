#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard pin layout on ESP32-S3 right header (avoids camera & PSRAM) */
#define LCD_PIN_SCLK  47  // SPI Clock
#define LCD_PIN_MOSI  21  // SPI MOSI (SDI / DIN)
#define LCD_PIN_DC    48  // Data/Command (RS)
#define LCD_PIN_RST   45  // Hardware Reset (RES)
#define LCD_PIN_CS    42  // Chip Select (or tie to GND)
#define LCD_PIN_BL    -1  // Backlight (-1 = tied to 3V3)

#define LCD_WIDTH     240
#define LCD_HEIGHT    240

// Common RGB565 colors
#define LCD_COLOR_BLACK   0x0000
#define LCD_COLOR_NAVY    0x000F
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_CYAN    0x07FF
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_MAGENTA 0xF81F
#define LCD_COLOR_YELLOW  0xFFE0
#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_GRAY    0x8410
#define LCD_COLOR_DARKGRAY 0x4208

/** Initialise SPI bus and ST7789 panel. Returns ESP_OK on success. */
esp_err_t lcd_st7789_init(void);

/** Fill the whole 240x240 screen with a single RGB565 color. */
void lcd_st7789_fill(uint16_t color);

/** Draw a filled rectangle. */
void lcd_st7789_fill_rect(int x, int y, int w, int h, uint16_t color);

/** Draw a string using built-in 8x16 font. */
void lcd_st7789_draw_string(int x, int y, const char *str, uint16_t fg, uint16_t bg);

/** Draw a string with 2x scaling (16x32 pixels per character). */
void lcd_st7789_draw_string_2x(int x, int y, const char *str, uint16_t fg, uint16_t bg);

/** Render system status dashboard on LCD. */
void lcd_st7789_show_dashboard(const char *ip, const char *ssid, bool usb_ok, uint32_t free_heap);

/** Draw colorful diagnostic test pattern (color bars). */
void lcd_st7789_test_pattern(void);

#ifdef __cplusplus
}
#endif
