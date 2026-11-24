#include "hal_st7735.h"
#include "font8x16.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

#define TAG "ST7735"

#define LCD_HOST        SPI2_HOST
#define PIN_NUM_MOSI    3
#define PIN_NUM_CLK     2
#define PIN_NUM_CS      7
#define PIN_NUM_DC      6
#define PIN_NUM_RST     10
#define PIN_NUM_BCKL    11

// Commands
#define ST7735_SWRESET  0x01
#define ST7735_SLPOUT   0x11
#define ST7735_NORON    0x13
#define ST7735_INVOFF   0x20
#define ST7735_INVON    0x21
#define ST7735_DISPON   0x29
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A

#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1

static spi_device_handle_t spi;

// Static DMA-aligned buffers to avoid runtime heap allocation / DMA misalign exceptions
#define CHUNK_PIXELS 64
static WORD_ALIGNED_ATTR DRAM_ATTR uint8_t s_chunk_buf[CHUNK_PIXELS * 2];
static WORD_ALIGNED_ATTR DRAM_ATTR uint8_t s_char_buf[8 * 16 * 2];

static void lcd_cmd(uint8_t cmd) {
    gpio_set_level(PIN_NUM_DC, 0);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = cmd;
    spi_device_polling_transmit(spi, &t);
}

static void lcd_data_byte(uint8_t data) {
    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = data;
    spi_device_polling_transmit(spi, &t);
}

static void lcd_data(const uint8_t *data, int len) {
    if (len == 0) return;
    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    if (len <= 4) {
        t.length = len * 8;
        t.flags = SPI_TRANS_USE_TXDATA;
        for (int i = 0; i < len; i++) t.tx_data[i] = data[i];
    } else {
        t.length = len * 8;
        t.tx_buffer = data;
    }
    spi_device_polling_transmit(spi, &t);
}

static void st7735_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += ST7735_XSTART;
    x1 += ST7735_XSTART;
    y0 += ST7735_YSTART;
    y1 += ST7735_YSTART;

    lcd_cmd(ST7735_CASET);
    uint8_t x_data[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    lcd_data(x_data, 4);

    lcd_cmd(ST7735_RASET);
    uint8_t y_data[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    lcd_data(y_data, 4);

    lcd_cmd(ST7735_RAMWR);
}

esp_err_t st7735_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_DC) | (1ULL << PIN_NUM_RST) | (1ULL << PIN_NUM_BCKL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Turn off backlight during init
    gpio_set_level(PIN_NUM_BCKL, 0);

    // Hardware reset
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Initialize SPI Master
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST7735_WIDTH * ST7735_HEIGHT * 2 + 8,
    };
    esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 26 * 1000 * 1000, // 26 MHz
        .mode = 0,                          // SPI mode 0
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
    };
    ret = spi_bus_add_device(LCD_HOST, &devcfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // ST7735S initialization sequence
    lcd_cmd(ST7735_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(ST7735_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Frame rate control
    lcd_cmd(ST7735_FRMCTR1);
    uint8_t f1[] = { 0x01, 0x2C, 0x2D };
    lcd_data(f1, sizeof(f1));

    lcd_cmd(ST7735_FRMCTR2);
    uint8_t f2[] = { 0x01, 0x2C, 0x2D };
    lcd_data(f2, sizeof(f2));

    lcd_cmd(ST7735_FRMCTR3);
    uint8_t f3[] = { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D };
    lcd_data(f3, sizeof(f3));

    // Display Inversion Control
    lcd_cmd(ST7735_INVCTR);
    lcd_data_byte(0x07);

    // Power Control
    lcd_cmd(ST7735_PWCTR1);
    uint8_t p1[] = { 0xA2, 0x02, 0x84 };
    lcd_data(p1, sizeof(p1));

    lcd_cmd(ST7735_PWCTR2);
    lcd_data_byte(0xC5);

    lcd_cmd(ST7735_PWCTR3);
    uint8_t p3[] = { 0x0A, 0x00 };
    lcd_data(p3, sizeof(p3));

    lcd_cmd(ST7735_PWCTR4);
    uint8_t p4[] = { 0x8A, 0x2A };
    lcd_data(p4, sizeof(p4));

    lcd_cmd(ST7735_PWCTR5);
    uint8_t p5[] = { 0x8A, 0xEE };
    lcd_data(p5, sizeof(p5));

    lcd_cmd(ST7735_VMCTR1);
    lcd_data_byte(0x0E);

    // Invert display ON (Required for ST7735S 0.96 inch)
    lcd_cmd(ST7735_INVON);

    // Color mode: 16-bit / pixel
    lcd_cmd(ST7735_COLMOD);
    lcd_data_byte(0x05);

    // Orientation: Portrait (MX | MY = 0xC0)
    lcd_cmd(ST7735_MADCTL);
    lcd_data_byte(0xC0);

    // Gamma adjustments
    lcd_cmd(ST7735_GMCTRP1);
    uint8_t gp[] = { 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 };
    lcd_data(gp, sizeof(gp));

    lcd_cmd(ST7735_GMCTRN1);
    uint8_t gn[] = { 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 };
    lcd_data(gn, sizeof(gn));

    // Normal display on
    lcd_cmd(ST7735_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Main screen turn on
    lcd_cmd(ST7735_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Clear screen to black
    st7735_fill_screen(COLOR_BLACK);

    // Turn on backlight
    gpio_set_level(PIN_NUM_BCKL, 1);

    ESP_LOGI(TAG, "ST7735 initialized successfully (160x80 landscape)");
    return ESP_OK;
}

void st7735_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x >= ST7735_WIDTH || y >= ST7735_HEIGHT || w <= 0 || h <= 0) return;
    if (x + w > ST7735_WIDTH) w = ST7735_WIDTH - x;
    if (y + h > ST7735_HEIGHT) h = ST7735_HEIGHT - y;

    st7735_set_window(x, y, x + w - 1, y + h - 1);

    // Swap bytes for SPI transmission (Big Endian)
    uint8_t hi = (color >> 8) & 0xFF;
    uint8_t lo = color & 0xFF;

    for (int i = 0; i < CHUNK_PIXELS; i++) {
        s_chunk_buf[i * 2] = hi;
        s_chunk_buf[i * 2 + 1] = lo;
    }

    int total_pixels = w * h;
    gpio_set_level(PIN_NUM_DC, 1);
    while (total_pixels > 0) {
        int count = total_pixels > CHUNK_PIXELS ? CHUNK_PIXELS : total_pixels;
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = count * 16;
        t.tx_buffer = s_chunk_buf;
        spi_device_polling_transmit(spi, &t);
        total_pixels -= count;
    }
}

void st7735_fill_screen(uint16_t color) {
    st7735_fill_rect(0, 0, ST7735_WIDTH, ST7735_HEIGHT, color);
}

void st7735_draw_pixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || x >= ST7735_WIDTH || y < 0 || y >= ST7735_HEIGHT) return;
    st7735_set_window(x, y, x, y);
    uint8_t data[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    lcd_data(data, 2);
}

void st7735_draw_hline(int16_t x, int16_t y, int16_t w, uint16_t color) {
    st7735_fill_rect(x, y, w, 1, color);
}

void st7735_draw_vline(int16_t x, int16_t y, int16_t h, uint16_t color) {
    st7735_fill_rect(x, y, 1, h, color);
}

void st7735_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font8x16[c - 32];

    if (x + 8 <= 0 || x >= ST7735_WIDTH || y + 16 <= 0 || y >= ST7735_HEIGHT) return;

    st7735_set_window(x, y, x + 7, y + 15);

    int idx = 0;
    uint8_t c_hi = color >> 8, c_lo = color & 0xFF;
    uint8_t bg_hi = bg >> 8, bg_lo = bg & 0xFF;

    for (int row = 0; row < 16; row++) {
        uint8_t row_bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (row_bits & (0x80 >> col)) {
                s_char_buf[idx++] = c_hi;
                s_char_buf[idx++] = c_lo;
            } else {
                s_char_buf[idx++] = bg_hi;
                s_char_buf[idx++] = bg_lo;
            }
        }
    }

    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = sizeof(s_char_buf) * 8;
    t.tx_buffer = s_char_buf;
    spi_device_polling_transmit(spi, &t);
}

void st7735_draw_string(int16_t x, int16_t y, const char *str, uint16_t color, uint16_t bg) {
    while (*str) {
        st7735_draw_char(x, y, *str, color, bg);
        x += 8;
        str++;
    }
}

void st7735_draw_progress_bar(int16_t x, int16_t y, int16_t w, int16_t h, float percent, uint16_t bar_color) {
    // Outer border
    st7735_draw_hline(x, y, w, COLOR_DARKGREY);
    st7735_draw_hline(x, y + h - 1, w, COLOR_DARKGREY);
    st7735_draw_vline(x, y, h, COLOR_DARKGREY);
    st7735_draw_vline(x + w - 1, y, h, COLOR_DARKGREY);

    if (isnan(percent) || percent < 0.0f) percent = 0.0f;
    else if (percent > 100.0f) percent = 100.0f;

    int inner_w = w - 2;
    int inner_h = h - 2;
    int fill_w = (int)((inner_w * percent) / 100.0f);

    if (fill_w > 0) {
        st7735_fill_rect(x + 1, y + 1, fill_w, inner_h, bar_color);
    }
    if (inner_w - fill_w > 0) {
        st7735_fill_rect(x + 1 + fill_w, y + 1, inner_w - fill_w, inner_h, COLOR_BLACK);
    }
}
