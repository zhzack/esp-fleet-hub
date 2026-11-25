#include "hal_display.h"
#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_FEATURE_DISPLAY

#include <stdio.h>
#include <string.h>
#include "board_pins.h"
#include "font8x16.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_FEATURE_PC_MONITOR
#include "hal_st7735.h"
#include "mod_pc_monitor.h"
#endif

static const char *TAG = "hal_display";
static char s_lines[HAL_DISPLAY_MAX_LINES][40] = {{0}};
static int s_backlight = 100;
static bool s_ready = false;

#if !CONFIG_FEATURE_PC_MONITOR
static spi_device_handle_t s_spi = NULL;

static void disp_cmd(uint8_t cmd)
{
    if (!s_spi || BOARD_DISP_DC < 0) return;
    gpio_set_level(BOARD_DISP_DC, 0);
    spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA, .tx_data = { cmd } };
    spi_device_polling_transmit(s_spi, &t);
}

static void disp_data(const uint8_t *data, int len)
{
    if (!s_spi || BOARD_DISP_DC < 0 || len <= 0) return;
    gpio_set_level(BOARD_DISP_DC, 1);
    spi_transaction_t t = { .length = len * 8 };
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    } else {
        t.tx_buffer = data;
    }
    spi_device_polling_transmit(s_spi, &t);
}

static void disp_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t xd[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t yd[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    disp_cmd(0x2A); disp_data(xd, 4);
    disp_cmd(0x2B); disp_data(yd, 4);
    disp_cmd(0x2C);
}

static void disp_draw_char_spi(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (!s_spi) return;
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font8x16[(uint8_t)(c - 32)];
    static WORD_ALIGNED_ATTR DRAM_ATTR uint8_t buf[8 * 16 * 2];
    int idx = 0;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint16_t color = (bits & (0x80 >> col)) ? fg : bg;
            buf[idx++] = (uint8_t)(color >> 8);
            buf[idx++] = (uint8_t)(color & 0xFF);
        }
    }
    disp_set_window(x, y, x + 7, y + 15);
    disp_data(buf, sizeof(buf));
}

static void disp_fill_rect_spi(int x, int y, int w, int h, uint16_t color)
{
    if (!s_spi || w <= 0 || h <= 0) return;
    disp_set_window(x, y, x + w - 1, y + h - 1);
    static WORD_ALIGNED_ATTR DRAM_ATTR uint8_t line[64 * 2];
    for (int i = 0; i < 64; i++) {
        line[2 * i] = (uint8_t)(color >> 8);
        line[2 * i + 1] = (uint8_t)(color & 0xFF);
    }
    int total = w * h;
    while (total > 0) {
        int chunk = total > 64 ? 64 : total;
        disp_data(line, chunk * 2);
        total -= chunk;
    }
}
#endif

esp_err_t hal_display_init(void)
{
    if (s_ready) return ESP_OK;

#if CONFIG_FEATURE_PC_MONITOR
    // PC Monitor owns SPI2 + ST7735; display module delegates safely to it.
    s_ready = true;
    ESP_LOGI(TAG, "Display HAL attached to active ST7735 (PC Monitor)");
    return ESP_OK;
#else
    if (BOARD_DISP_MOSI < 0 || BOARD_DISP_CLK < 0 || BOARD_DISP_DC < 0) {
        ESP_LOGW(TAG, "No standalone SPI display pins configured on this board; running virtual frame buffer.");
        s_ready = true;
        return ESP_OK;
    }

    uint64_t mask = (1ULL << BOARD_DISP_DC);
    if (BOARD_DISP_RST >= 0)  mask |= (1ULL << BOARD_DISP_RST);
    if (BOARD_DISP_BCKL >= 0) mask |= (1ULL << BOARD_DISP_BCKL);
    gpio_config_t io = { .pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    if (BOARD_DISP_BCKL >= 0) gpio_set_level(BOARD_DISP_BCKL, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_DISP_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_DISP_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI2 display bus init skipped (%s), virtual mode enabled", esp_err_to_name(err));
        s_ready = true;
        return ESP_OK;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = BOARD_DISP_CS,
        .queue_size = 4,
    };
    if (spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi) != ESP_OK) {
        s_ready = true;
        return ESP_OK;
    }

    if (BOARD_DISP_RST >= 0) {
        gpio_set_level(BOARD_DISP_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(BOARD_DISP_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    }
    disp_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(60));
    disp_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(60));
    uint8_t col = 0x55; disp_cmd(0x3A); disp_data(&col, 1);
    uint8_t mad = 0x00; disp_cmd(0x36); disp_data(&mad, 1);
    disp_cmd(0x21); // ST7789 inversion on
    disp_cmd(0x29); // Display ON
    disp_fill_rect_spi(0, 0, 240, 240, 0x0000);

    s_ready = true;
    ESP_LOGI(TAG, "ST7789/SPI Display HAL ready (MOSI=%d CLK=%d CS=%d DC=%d)",
             BOARD_DISP_MOSI, BOARD_DISP_CLK, BOARD_DISP_CS, BOARD_DISP_DC);
    return ESP_OK;
#endif
}

esp_err_t hal_display_set_line(int row, const char *text)
{
    if (row < 0 || row >= HAL_DISPLAY_MAX_LINES || !text) return ESP_ERR_INVALID_ARG;
    strlcpy(s_lines[row], text, sizeof(s_lines[row]));

#if CONFIG_FEATURE_PC_MONITOR
    (void)0;
#else
    if (s_spi) {
        int y = 8 + row * 24;
        disp_fill_rect_spi(0, y, 240, 18, 0x0000);
        for (int i = 0; s_lines[row][i] && i < 28; i++) {
            disp_draw_char_spi(4 + i * 8, y, s_lines[row][i], 0xFFFF, 0x0000);
        }
    }
#endif
    return ESP_OK;
}

esp_err_t hal_display_clear(void)
{
    memset(s_lines, 0, sizeof(s_lines));
#if CONFIG_FEATURE_PC_MONITOR
    st7735_fill_screen(COLOR_BLACK);
#else
    if (s_spi) disp_fill_rect_spi(0, 0, 240, 240, 0x0000);
#endif
    return ESP_OK;
}

esp_err_t hal_display_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_backlight = percent;
#if CONFIG_FEATURE_PC_MONITOR
    st7735_set_backlight(percent);
#else
    if (BOARD_DISP_BCKL >= 0) gpio_set_level(BOARD_DISP_BCKL, percent > 0 ? 1 : 0);
#endif
    return ESP_OK;
}

void hal_display_apply_config(void)
{
#if CONFIG_FEATURE_PC_MONITOR
    mod_pc_monitor_apply_config();
#else
    app_config_t cfg;
    app_config_get(&cfg);
    hal_display_set_backlight(strcmp(cfg.display.mode, "off") == 0 ? 0 : cfg.display.backlight);
    for (int i = 0; i < HAL_DISPLAY_MAX_LINES; i++) {
        hal_display_set_line(i, cfg.display.lines[i]);
    }
#endif
}

const char *hal_display_get_line(int row)
{
    if (row < 0 || row >= HAL_DISPLAY_MAX_LINES) return "";
    return s_lines[row];
}

int hal_display_get_backlight(void) { return s_backlight; }
bool hal_display_is_ready(void) { return s_ready; }
const char *hal_display_driver_name(void)
{
#if CONFIG_FEATURE_PC_MONITOR
    return "ST7735_SHARED";
#else
    return s_spi ? "ST7789_SPI" : "VIRTUAL_FRAMEBUF";
#endif
}

#else

esp_err_t hal_display_init(void) { return ESP_OK; }
esp_err_t hal_display_set_line(int row, const char *text) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t hal_display_clear(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t hal_display_set_backlight(int percent) { return ESP_ERR_NOT_SUPPORTED; }
void hal_display_apply_config(void) {}
const char *hal_display_get_line(int row) { return ""; }
int hal_display_get_backlight(void) { return 0; }
bool hal_display_is_ready(void) { return false; }
const char *hal_display_driver_name(void) { return "DISABLED"; }

#endif
