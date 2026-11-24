#ifndef ST7735_H
#define ST7735_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Screen physical dimensions in portrait mode
#define ST7735_WIDTH   80
#define ST7735_HEIGHT  160

// ST7735S 80x160 Portrait offsets
#define ST7735_XSTART  26
#define ST7735_YSTART  1

// RGB565 Color definitions
#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_RED         0xF800
#define COLOR_GREEN       0x07E0
#define COLOR_BLUE        0x001F
#define COLOR_CYAN        0x07FF
#define COLOR_MAGENTA     0xF81F
#define COLOR_YELLOW      0xFFE0
#define COLOR_ORANGE      0xFD20
#define COLOR_NAVY        0x000F
#define COLOR_DARKGREY    0x4208
#define COLOR_LIGHTGREY   0xC618

// Initialize SPI master and ST7735 controller
esp_err_t st7735_init(void);

// Primitive graphics
void st7735_fill_screen(uint16_t color);
void st7735_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void st7735_draw_pixel(int16_t x, int16_t y, uint16_t color);
void st7735_draw_hline(int16_t x, int16_t y, int16_t w, uint16_t color);
void st7735_draw_vline(int16_t x, int16_t y, int16_t h, uint16_t color);

// Text & UI components
void st7735_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg);
void st7735_draw_string(int16_t x, int16_t y, const char *str, uint16_t color, uint16_t bg);
void st7735_draw_progress_bar(int16_t x, int16_t y, int16_t w, int16_t h, float percent, uint16_t bar_color);

#endif // ST7735_H
