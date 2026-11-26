#include "hal_epd.h"
#include "sdkconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"

#if CONFIG_FEATURE_DISPLAY && !CONFIG_FEATURE_PC_MONITOR

#include "board_pins.h"
#include "cJSON.h"
#include "font8x16.h"
#include "hal_storage.h"
#include "mbedtls/base64.h"
#include "system_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hal_epd";

#define EPD_MAX_BUF_SIZE     15000 // 400 * 300 / 8
#define EPD_BW213_BUF_SIZE   4000  // 128 * 250 / 8

typedef enum {
    EPD_WHITE = 0,
    EPD_BLACK = 1,
    EPD_RED   = 2,
} epd_color_t;

typedef struct {
    const char *id;
    const char *driver;
    uint16_t ram_w;
    uint16_t vis_w;
    uint16_t vis_h;
    bool has_red;
    bool supports_partial;
    uint8_t sleep_arg;
    uint8_t pwroff_opt;
} epd_panel_spec_t;

static const epd_panel_spec_t s_specs[] = {
    [HAL_EPD_PANEL_213_BW] = {
        .id = "epd_213_bw",
        .driver = "SSD1683_EPD_213_BW",
        .ram_w = 128,
        .vis_w = 122,
        .vis_h = 250,
        .has_red = false,
        .supports_partial = true,
        .sleep_arg = 0x01,
        .pwroff_opt = 0x83,
    },
    [HAL_EPD_PANEL_213_BWR] = {
        .id = "epd_213_bwr",
        .driver = "SSD1683_EPD_213_BWR",
        .ram_w = 128,
        .vis_w = 122,
        .vis_h = 250,
        .has_red = true,
        .supports_partial = false,
        .sleep_arg = 0x01,
        .pwroff_opt = 0x83,
    },
    [HAL_EPD_PANEL_420_BWR] = {
        .id = "epd_420_bwr",
        .driver = "SSD1683_EPD_420_BWR",
        .ram_w = 400,
        .vis_w = 400,
        .vis_h = 300,
        .has_red = true,
        .supports_partial = false,
        .sleep_arg = 0x11,
        .pwroff_opt = 0xC3,
    },
};

typedef struct {
    float cpu;
    float ram;
    float disk;
    char dl[20];
    char ul[20];
    char uptime[16];
    char time_str[16];
    int64_t updated_ms;
} epd_metrics_t;

#define EPD_IMG_FILE_PATH "/storage/epd_img.bin"
#define EPD_IMG_MAGIC     0x49445045U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t has_red;
    uint8_t reserved[3];
    uint32_t plane_len;
} epd_img_file_hdr_t;

static spi_device_handle_t s_spi = NULL;
static bool s_bus_owned = false;
static bool s_initialized = false;
static SemaphoreHandle_t s_lock = NULL;
static SemaphoreHandle_t s_img_lock = NULL;
static TaskHandle_t s_worker = NULL;

static uint8_t *s_bw_buf = NULL;
static uint8_t *s_red_buf = NULL;
static uint8_t *s_prev_bw_buf = NULL;

static uint8_t *s_img_bw = NULL;
static uint8_t *s_img_red = NULL;
static uint16_t s_img_w = 0;
static uint16_t s_img_h = 0;
static bool s_img_has_red = false;
static bool s_img_loaded = false;
static volatile bool s_img_dirty = false;

static int s_pin_mosi = -1;
static int s_pin_clk = -1;
static int s_pin_cs = -1;
static int s_pin_dc = -1;
static int s_pin_rst = -1;
static int s_pin_busy = -1;

static volatile bool s_refreshing = false;
static volatile bool s_busy_timeout = false;
static volatile bool s_pending_full = false;
static volatile bool s_pending_clear = false;
static volatile bool s_pending_redraw = false;
static volatile uint32_t s_refresh_count = 0;
static volatile uint32_t s_last_refresh_ms = 0;

static bool s_panel_painted = false;
static hal_epd_panel_t s_last_painted_panel = HAL_EPD_PANEL_213_BWR;
static uint32_t s_partial_streak = 0;

static epd_metrics_t s_metrics = {
    .cpu = 0.0f,
    .ram = 0.0f,
    .disk = 0.0f,
    .dl = "0K/s",
    .ul = "0K/s",
    .uptime = "--",
    .time_str = "--:--",
    .updated_ms = 0,
};

static bool ensure_img_buffers_locked(void)
{
    if (!s_img_bw) {
        s_img_bw = calloc(1, EPD_MAX_BUF_SIZE);
    }
    if (!s_img_red) {
        s_img_red = calloc(1, EPD_MAX_BUF_SIZE);
    }
    return (s_img_bw != NULL && s_img_red != NULL);
}

static bool decode_packbits(const uint8_t *in, size_t in_len, uint8_t *out, size_t expected_len)
{
    size_t ip = 0;
    size_t op = 0;
    while (ip < in_len && op < expected_len) {
        int8_t hdr = (int8_t)in[ip++];
        if (hdr >= 0) {
            size_t cnt = (size_t)hdr + 1U;
            if (ip + cnt > in_len || op + cnt > expected_len) return false;
            memcpy(out + op, in + ip, cnt);
            ip += cnt;
            op += cnt;
        } else if (hdr != -128) {
            size_t cnt = (size_t)(1 - (int)hdr);
            if (ip >= in_len || op + cnt > expected_len) return false;
            memset(out + op, in[ip++], cnt);
            op += cnt;
        }
    }
    return (op == expected_len);
}

static bool decode_plane_b64(const char *b64_str, bool use_rle, size_t expected_len, uint8_t *out_buf)
{
    if (!b64_str || !b64_str[0] || !out_buf || expected_len == 0 || expected_len > EPD_MAX_BUF_SIZE) {
        return false;
    }
    size_t b64_len = strlen(b64_str);
    size_t raw_cap = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &raw_cap, (const unsigned char *)b64_str, b64_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) return false;
    if (raw_cap == 0 || raw_cap > (EPD_MAX_BUF_SIZE + 1024U)) return false;

    uint8_t *raw = malloc(raw_cap);
    if (!raw) return false;
    size_t raw_len = 0;
    rc = mbedtls_base64_decode(raw, raw_cap, &raw_len, (const unsigned char *)b64_str, b64_len);
    bool ok = false;
    if (rc == 0) {
        if (use_rle) {
            ok = decode_packbits(raw, raw_len, out_buf, expected_len);
        } else if (raw_len == expected_len) {
            memcpy(out_buf, raw, expected_len);
            ok = true;
        }
    }
    free(raw);
    return ok;
}

static void load_image_from_storage_if_needed(void)
{
    if (s_img_loaded || !hal_storage_is_mounted() || !s_img_lock) return;
    FILE *f = fopen(EPD_IMG_FILE_PATH, "rb");
    if (!f) {
        s_img_loaded = true;
        return;
    }

    epd_img_file_hdr_t hdr = {0};
    bool valid = (fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
                  hdr.magic == EPD_IMG_MAGIC &&
                  hdr.width >= 1 && hdr.width <= 400 &&
                  hdr.height >= 1 && hdr.height <= 400);
    size_t expected = valid ? (((size_t)hdr.width + 7U) / 8U) * (size_t)hdr.height : 0;
    if (!valid || hdr.plane_len != expected || expected == 0 || expected > EPD_MAX_BUF_SIZE) {
        fclose(f);
        s_img_loaded = true;
        return;
    }

    uint8_t *bw = calloc(1, EPD_MAX_BUF_SIZE);
    uint8_t *red = calloc(1, EPD_MAX_BUF_SIZE);
    if (!bw || !red) {
        free(bw);
        free(red);
        fclose(f);
        return;
    }
    memset(bw, 0xFF, EPD_MAX_BUF_SIZE);
    bool ok = (fread(bw, 1, expected, f) == expected);
    if (ok && hdr.has_red) {
        ok = (fread(red, 1, expected, f) == expected);
    }
    fclose(f);

    if (ok && xSemaphoreTake(s_img_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (ensure_img_buffers_locked()) {
            memcpy(s_img_bw, bw, expected);
            memcpy(s_img_red, red, expected);
            s_img_w = hdr.width;
            s_img_h = hdr.height;
            s_img_has_red = (hdr.has_red != 0);
        }
        xSemaphoreGive(s_img_lock);
    }
    free(bw);
    free(red);
    s_img_loaded = true;
}

static void save_dirty_image_to_storage(void)
{
    if (!s_img_dirty || !hal_storage_is_mounted() || !s_img_lock) return;

    epd_img_file_hdr_t hdr = {0};
    uint8_t *bw_copy = malloc(EPD_MAX_BUF_SIZE);
    uint8_t *red_copy = malloc(EPD_MAX_BUF_SIZE);
    if (!bw_copy || !red_copy) {
        free(bw_copy);
        free(red_copy);
        return;
    }

    size_t plen = 0;
    if (xSemaphoreTake(s_img_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_img_bw && s_img_w >= 1 && s_img_h >= 1) {
            plen = (((size_t)s_img_w + 7U) / 8U) * (size_t)s_img_h;
            if (plen <= EPD_MAX_BUF_SIZE) {
                hdr.magic = EPD_IMG_MAGIC;
                hdr.width = s_img_w;
                hdr.height = s_img_h;
                hdr.has_red = s_img_has_red ? 1 : 0;
                hdr.plane_len = (uint32_t)plen;
                memcpy(bw_copy, s_img_bw, plen);
                if (s_img_has_red && s_img_red) {
                    memcpy(red_copy, s_img_red, plen);
                }
            }
        }
        s_img_dirty = false;
        xSemaphoreGive(s_img_lock);
    }

    if (hdr.magic == EPD_IMG_MAGIC && plen > 0) {
        FILE *f = fopen(EPD_IMG_FILE_PATH, "wb");
        if (f) {
            fwrite(&hdr, 1, sizeof(hdr), f);
            fwrite(bw_copy, 1, plen, f);
            if (hdr.has_red) {
                fwrite(red_copy, 1, plen, f);
            }
            fclose(f);
            ESP_LOGI(TAG, "Saved EPD image (%ux%u, %s, %u B/plane) to %s",
                     hdr.width, hdr.height, hdr.has_red ? "bwr" : "bw",
                     (unsigned)plen, EPD_IMG_FILE_PATH);
        }
    }
    free(bw_copy);
    free(red_copy);
}

bool hal_epd_is_epd_panel_name(const char *panel_name)
{
    if (!panel_name || !panel_name[0]) return false;
    return (strcmp(panel_name, "epd_213_bw") == 0 ||
            strcmp(panel_name, "epd_213_bwr") == 0 ||
            strcmp(panel_name, "epd_420_bwr") == 0);
}

hal_epd_panel_t hal_epd_parse_panel(const char *panel_name)
{
    if (panel_name) {
        if (strcmp(panel_name, "epd_213_bw") == 0)  return HAL_EPD_PANEL_213_BW;
        if (strcmp(panel_name, "epd_420_bwr") == 0) return HAL_EPD_PANEL_420_BWR;
        if (strcmp(panel_name, "epd_213_bwr") == 0) return HAL_EPD_PANEL_213_BWR;
    }
    return HAL_EPD_PANEL_213_BWR;
}

const char *hal_epd_panel_name(hal_epd_panel_t panel)
{
    if (panel >= HAL_EPD_PANEL_213_BW && panel <= HAL_EPD_PANEL_420_BWR) {
        return s_specs[panel].id;
    }
    return s_specs[HAL_EPD_PANEL_213_BWR].id;
}

static void resolve_pins(const app_display_config_t *dcfg,
                         int *mosi, int *clk, int *cs, int *dc, int *rst, int *busy)
{
    *mosi = (dcfg && dcfg->pin_mosi >= 0) ? dcfg->pin_mosi : BOARD_DISP_MOSI;
    *clk  = (dcfg && dcfg->pin_clk  >= 0) ? dcfg->pin_clk  : BOARD_DISP_CLK;
    *cs   = (dcfg && dcfg->pin_cs   >= 0) ? dcfg->pin_cs   : BOARD_DISP_CS;
    *dc   = (dcfg && dcfg->pin_dc   >= 0) ? dcfg->pin_dc   : BOARD_DISP_DC;
    *rst  = (dcfg && dcfg->pin_rst  >= 0) ? dcfg->pin_rst  : BOARD_DISP_RST;
    *busy = (dcfg && dcfg->pin_busy >= 0) ? dcfg->pin_busy : BOARD_DISP_BUSY;
}

/* ==================== Low-Level SSD1680/SSD1683 SPI Transport ==================== */

static void epd_cmd(uint8_t cmd)
{
    if (!s_spi || s_pin_dc < 0) return;
    gpio_set_level(s_pin_dc, 0);
    spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA, .tx_data = { cmd } };
    spi_device_polling_transmit(s_spi, &t);
}

static void epd_data_byte(uint8_t b)
{
    if (!s_spi || s_pin_dc < 0) return;
    gpio_set_level(s_pin_dc, 1);
    spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA, .tx_data = { b } };
    spi_device_polling_transmit(s_spi, &t);
}

static void epd_data(const uint8_t *buf, size_t len)
{
    if (!s_spi || s_pin_dc < 0 || !buf || len == 0) return;
    gpio_set_level(s_pin_dc, 1);
    if (len <= 4) {
        spi_transaction_t t = { .length = len * 8, .flags = SPI_TRANS_USE_TXDATA };
        memcpy(t.tx_data, buf, len);
        spi_device_polling_transmit(s_spi, &t);
        return;
    }
    spi_transaction_t t = { .length = len * 8, .tx_buffer = buf };
    spi_device_polling_transmit(s_spi, &t);
}

static bool epd_wait_busy(const char *stage, uint32_t timeout_ms)
{
    if (s_pin_busy < 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms > 200 ? 200 : timeout_ms));
        return true;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    vTaskDelay(pdMS_TO_TICKS(5));
    while (gpio_get_level(s_pin_busy) == 1) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "BUSY wait timeout (%ums) at %s (BUSY GPIO%d still HIGH)",
                     (unsigned)timeout_ms, stage ? stage : "?", s_pin_busy);
            s_busy_timeout = true;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static void epd_hw_reset(void)
{
    if (s_pin_rst < 0) return;
    gpio_set_level(s_pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void epd_set_ram_window(uint16_t ram_w, uint16_t vis_h)
{
    uint8_t x_end = (uint8_t)((ram_w / 8) - 1);
    uint16_t y_end = vis_h - 1;

    epd_cmd(0x11); // Data entry mode: X inc, Y inc (AM=0)
    epd_data_byte(0x03);

    epd_cmd(0x44); // RAM X start / end
    uint8_t xd[2] = { 0x00, x_end };
    epd_data(xd, 2);

    epd_cmd(0x45); // RAM Y start / end
    uint8_t yd[4] = { 0x00, 0x00, (uint8_t)(y_end & 0xFF), (uint8_t)(y_end >> 8) };
    epd_data(yd, 4);

    epd_cmd(0x4E); // X address counter = 0
    epd_data_byte(0x00);

    epd_cmd(0x4F); // Y address counter = 0
    uint8_t yc[2] = { 0x00, 0x00 };
    epd_data(yc, 2);
}

static void epd_init_controller(const epd_panel_spec_t *spec, hal_epd_panel_t panel_id)
{
    epd_hw_reset();
    epd_wait_busy("hw_reset", 1500);

    epd_cmd(0x12); // SWRESET
    vTaskDelay(pdMS_TO_TICKS(12));
    epd_wait_busy("swreset", 1500);

    uint16_t gates = spec->vis_h - 1;
    epd_cmd(0x01); // Driver output control
    uint8_t drv[3] = { (uint8_t)(gates & 0xFF), (uint8_t)(gates >> 8), 0x00 };
    epd_data(drv, 3);

    epd_cmd(0x3C); // Border waveform
    epd_data_byte(0x05);

    if (panel_id != HAL_EPD_PANEL_420_BWR) {
        // GDEY0213B74 & GDEY0213Z98 explicitly configure Display Update Control 1
        epd_cmd(0x21);
        uint8_t duc1[2] = { 0x00, 0x80 };
        epd_data(duc1, 2);
    }

    epd_cmd(0x18); // Built-in temperature sensor
    epd_data_byte(0x80);

    epd_set_ram_window(spec->ram_w, spec->vis_h);
}

static void epd_hibernate(const epd_panel_spec_t *spec)
{
    epd_cmd(0x22);
    epd_data_byte(spec->pwroff_opt);
    epd_cmd(0x20);
    epd_wait_busy("power_off", 2500);

    epd_cmd(0x10);
    epd_data_byte(spec->sleep_arg);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void epd_flush_to_hardware(hal_epd_panel_t panel_id, bool use_partial)
{
    if (!s_spi || !s_bw_buf || !s_red_buf) return;
    const epd_panel_spec_t *spec = &s_specs[panel_id];
    size_t buf_len = ((size_t)spec->ram_w / 8U) * (size_t)spec->vis_h;

    s_busy_timeout = false;
    int64_t t0 = esp_timer_get_time();

    epd_init_controller(spec, panel_id);

    if (!spec->has_red) {
        if (use_partial && s_prev_bw_buf && buf_len <= EPD_BW213_BUF_SIZE) {
            // Re-seed previous frame after deep-sleep wake, write new frame, run 0xFC partial LUT
            epd_set_ram_window(spec->ram_w, spec->vis_h);
            epd_cmd(0x26);
            epd_data(s_prev_bw_buf, buf_len);

            epd_set_ram_window(spec->ram_w, spec->vis_h);
            epd_cmd(0x24);
            epd_data(s_bw_buf, buf_len);

            epd_cmd(0x3C);
            epd_data_byte(0x80); // Lock border during partial update

            epd_cmd(0x22);
            epd_data_byte(0xFC);
            epd_cmd(0x20);
            epd_wait_busy("update_part", 6000);
        } else {
            // Standard B/W full refresh: sync 0x26 & 0x24 to identical image
            epd_set_ram_window(spec->ram_w, spec->vis_h);
            epd_cmd(0x26);
            epd_data(s_bw_buf, buf_len);

            epd_set_ram_window(spec->ram_w, spec->vis_h);
            epd_cmd(0x24);
            epd_data(s_bw_buf, buf_len);

            epd_cmd(0x22);
            epd_data_byte(0xF7);
            epd_cmd(0x20);
            epd_wait_busy("update_full_bw", 12000);
            s_partial_streak = 0;
        }
        if (s_prev_bw_buf && buf_len <= EPD_BW213_BUF_SIZE) {
            memcpy(s_prev_bw_buf, s_bw_buf, buf_len);
        }
    } else {
        // Tri-color SSD1683/SSD1680: 0x24 = BW (1=White, 0=Black), 0x26 = Red (1=Red, 0=None)
        epd_set_ram_window(spec->ram_w, spec->vis_h);
        epd_cmd(0x24);
        epd_data(s_bw_buf, buf_len);

        epd_set_ram_window(spec->ram_w, spec->vis_h);
        epd_cmd(0x26);
        epd_data(s_red_buf, buf_len);

        epd_cmd(0x22);
        epd_data_byte(0xF7);
        epd_cmd(0x20);
        epd_wait_busy("update_full_bwr", 25000);
        s_partial_streak = 0;
    }

    epd_hibernate(spec);

    s_last_refresh_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000LL);
    s_refresh_count++;
    s_panel_painted = true;
    s_last_painted_panel = panel_id;
    ESP_LOGI(TAG, "EPD [%s] refresh #%lu complete in %lu ms%s",
             spec->id, (unsigned long)s_refresh_count, (unsigned long)s_last_refresh_ms,
             s_busy_timeout ? " (BUSY timeout!)" : "");
}

/* ==================== Adaptive 3-Color Canvas & Layout Renderer ==================== */

typedef struct {
    const epd_panel_spec_t *spec;
    uint16_t rot;
    uint16_t cw;
    uint16_t ch;
    bool invert;
} epd_canvas_t;

static void canvas_begin(epd_canvas_t *c, hal_epd_panel_t panel_id, uint16_t rot, bool invert)
{
    c->spec = &s_specs[panel_id];
    c->rot = (rot == 90 || rot == 180 || rot == 270) ? rot : 0;
    c->cw = (c->rot == 90 || c->rot == 270) ? c->spec->vis_h : c->spec->vis_w;
    c->ch = (c->rot == 90 || c->rot == 270) ? c->spec->vis_w : c->spec->vis_h;
    c->invert = invert;

    size_t buf_len = ((size_t)c->spec->ram_w / 8U) * (size_t)c->spec->vis_h;
    memset(s_bw_buf, invert ? 0x00 : 0xFF, buf_len);
    memset(s_red_buf, 0x00, buf_len);
}

static void canvas_set_px(const epd_canvas_t *c, int x, int y, epd_color_t color)
{
    if (!c || x < 0 || y < 0 || x >= (int)c->cw || y >= (int)c->ch) return;

    int px = x, py = y;
    int nw = c->spec->vis_w;
    int nh = c->spec->vis_h;
    switch (c->rot) {
        case 90:
            px = nw - 1 - y;
            py = x;
            break;
        case 180:
            px = nw - 1 - x;
            py = nh - 1 - y;
            break;
        case 270:
            px = y;
            py = nh - 1 - x;
            break;
        default:
            break;
    }
    if (px < 0 || py < 0 || px >= nw || py >= nh) return;

    epd_color_t resolved = color;
    if (resolved == EPD_RED && !c->spec->has_red) {
        resolved = EPD_BLACK;
    }
    if (c->invert) {
        if (resolved == EPD_WHITE) resolved = EPD_BLACK;
        else if (resolved == EPD_BLACK) resolved = EPD_WHITE;
    }

    size_t stride = (size_t)c->spec->ram_w / 8U;
    size_t idx = (size_t)py * stride + (size_t)(px >> 3);
    uint8_t bit = (uint8_t)(0x80U >> (px & 7));

    if (resolved == EPD_WHITE) {
        s_bw_buf[idx] |= bit;
        s_red_buf[idx] &= (uint8_t)~bit;
    } else if (resolved == EPD_BLACK) {
        s_bw_buf[idx] &= (uint8_t)~bit;
        s_red_buf[idx] &= (uint8_t)~bit;
    } else { // EPD_RED
        s_bw_buf[idx] |= bit;
        s_red_buf[idx] |= bit;
    }
}

static void canvas_fill_rect(const epd_canvas_t *c, int x, int y, int w, int h, epd_color_t color)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            canvas_set_px(c, i, j, color);
        }
    }
}

static void canvas_draw_rect(const epd_canvas_t *c, int x, int y, int w, int h, epd_color_t color)
{
    if (w <= 0 || h <= 0) return;
    canvas_fill_rect(c, x, y, w, 1, color);
    canvas_fill_rect(c, x, y + h - 1, w, 1, color);
    canvas_fill_rect(c, x, y, 1, h, color);
    canvas_fill_rect(c, x + w - 1, y, 1, h, color);
}

static void canvas_draw_char(const epd_canvas_t *c, int x, int y, char ch, epd_color_t fg, int scale)
{
    if (scale < 1) scale = 1;
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font8x16[(uint8_t)(ch - 32)];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                if (scale == 1) {
                    canvas_set_px(c, x + col, y + row, fg);
                } else {
                    canvas_fill_rect(c, x + col * scale, y + row * scale, scale, scale, fg);
                }
            }
        }
    }
}

static void canvas_draw_str(const epd_canvas_t *c, int x, int y, const char *s, epd_color_t fg, int scale)
{
    if (!s) return;
    if (scale < 1) scale = 1;
    int adv = 8 * scale;
    for (int i = 0; s[i]; i++) {
        if (x + adv > (int)c->cw) break;
        canvas_draw_char(c, x, y, s[i], fg, scale);
        x += adv;
    }
}

static void canvas_draw_str_right(const epd_canvas_t *c, int right_x, int y, const char *s, epd_color_t fg, int scale)
{
    if (!s) return;
    if (scale < 1) scale = 1;
    int adv = 8 * scale;
    int w = (int)strlen(s) * adv;
    int start_x = right_x - w;
    if (start_x < 0) start_x = 0;
    canvas_draw_str(c, start_x, y, s, fg, scale);
}

static void canvas_draw_progress(const epd_canvas_t *c, int x, int y, int w, int h, float pct, epd_color_t fill_col)
{
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    canvas_draw_rect(c, x, y, w, h, EPD_BLACK);
    int inner_w = w - 4;
    int fill_w = (int)((pct / 100.0f) * inner_w);
    if (fill_w > 0 && h > 4) {
        canvas_fill_rect(c, x + 2, y + 2, fill_w, h - 4, fill_col);
    }
}

static const char *strip_red_tag(const char *src, bool *has_tag)
{
    *has_tag = false;
    if (!src) return "";
    if (strncasecmp(src, "[R]", 3) == 0) {
        *has_tag = true;
        src += 3;
        while (*src == ' ') src++;
    } else if (strncasecmp(src, "[RED]", 5) == 0) {
        *has_tag = true;
        src += 5;
        while (*src == ' ') src++;
    }
    return src;
}

static void format_uptime(uint32_t sec, char *dst, size_t cap)
{
    if (sec < 60) {
        snprintf(dst, cap, "%lus", (unsigned long)sec);
    } else if (sec < 3600) {
        snprintf(dst, cap, "%lum%lus", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    } else {
        snprintf(dst, cap, "%luh%lum", (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60));
    }
}

static void render_image_mode(const epd_canvas_t *c)
{
    bool painted = false;
    if (s_img_lock && xSemaphoreTake(s_img_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_img_bw && s_img_w > 0 && s_img_h > 0) {
            size_t stride = ((size_t)s_img_w + 7U) / 8U;
            if (s_img_w == c->cw && s_img_h == c->ch) {
                for (int y = 0; y < (int)c->ch; y++) {
                    size_t row_off = (size_t)y * stride;
                    for (int x = 0; x < (int)c->cw; x++) {
                        size_t idx = row_off + (size_t)(x >> 3);
                        uint8_t bit = (uint8_t)(0x80U >> (x & 7));
                        bool is_red = (s_img_has_red && s_img_red && (s_img_red[idx] & bit) != 0);
                        bool is_white = ((s_img_bw[idx] & bit) != 0);
                        epd_color_t col = is_red ? EPD_RED : (is_white ? EPD_WHITE : EPD_BLACK);
                        canvas_set_px(c, x, y, col);
                    }
                }
            } else {
                // Aspect-preserving centered scaling if rotation/panel changed after image upload
                uint32_t sx_num = (uint32_t)c->cw * (uint32_t)s_img_h;
                uint32_t sy_num = (uint32_t)c->ch * (uint32_t)s_img_w;
                int draw_w = (sx_num <= sy_num) ? (int)c->cw : (int)(sy_num / s_img_h);
                int draw_h = (sx_num <= sy_num) ? (int)((uint32_t)c->cw * s_img_h / s_img_w) : (int)c->ch;
                if (draw_w < 1) draw_w = 1;
                if (draw_h < 1) draw_h = 1;
                int ox = ((int)c->cw - draw_w) / 2;
                int oy = ((int)c->ch - draw_h) / 2;
                for (int dy = 0; dy < draw_h; dy++) {
                    int sy = (dy * (int)s_img_h) / draw_h;
                    if (sy >= (int)s_img_h) sy = (int)s_img_h - 1;
                    size_t row_off = (size_t)sy * stride;
                    for (int dx = 0; dx < draw_w; dx++) {
                        int sx = (dx * (int)s_img_w) / draw_w;
                        if (sx >= (int)s_img_w) sx = (int)s_img_w - 1;
                        size_t idx = row_off + (size_t)(sx >> 3);
                        uint8_t bit = (uint8_t)(0x80U >> (sx & 7));
                        bool is_red = (s_img_has_red && s_img_red && (s_img_red[idx] & bit) != 0);
                        bool is_white = ((s_img_bw[idx] & bit) != 0);
                        epd_color_t col = is_red ? EPD_RED : (is_white ? EPD_WHITE : EPD_BLACK);
                        canvas_set_px(c, ox + dx, oy + dy, col);
                    }
                }
            }
            painted = true;
        }
        xSemaphoreGive(s_img_lock);
    }

    if (!painted) {
        int cy = (int)c->ch / 2 - 20;
        if (cy < 4) cy = 4;
        canvas_draw_rect(c, 4, cy - 6, c->cw - 8, 52, EPD_BLACK);
        canvas_draw_str(c, 12, cy, "IMAGE MODE", c->spec->has_red ? EPD_RED : EPD_BLACK, 1);
        canvas_draw_str(c, 12, cy + 20, "No image loaded", EPD_BLACK, 1);
    }
}

static void render_epd_frame(const app_config_t *cfg, const epd_canvas_t *c, hal_epd_panel_t panel_id)
{
    const app_display_config_t *d = &cfg->display;
    if (strcmp(d->mode, "off") == 0) {
        return; // Clean blank white frame already prepared by canvas_begin
    }
    if (strcmp(d->mode, "image") == 0) {
        render_image_mode(c);
        return;
    }

    bool is_large = (c->cw >= 300);
    bool is_wide  = (c->cw > c->ch);
    int pad = is_large ? 10 : 4;
    int hdr_h = is_large ? 38 : 22;
    int ftr_h = is_large ? 22 : 16;

    bool red_hdr = c->spec->has_red && (((d->red_mask & 0x40) != 0) || strcmp(d->theme, "red_bar") == 0);
    if (strcmp(d->theme, "mono") == 0) red_hdr = false;
    epd_color_t hdr_bg = red_hdr ? EPD_RED : EPD_BLACK;

    // 1. Header Bar
    canvas_fill_rect(c, 0, 0, c->cw, hdr_h, hdr_bg);
    const char *hdr_txt = d->header_text[0] ? d->header_text : (cfg->device_id[0] ? cfg->device_id : "ESP32-S3");
    int hdr_scale = is_large ? 2 : 1;
    int hdr_y = (hdr_h - 16 * hdr_scale) / 2;
    canvas_draw_str(c, pad, hdr_y, hdr_txt, EPD_WHITE, hdr_scale);

    const char *badge = (strcmp(d->mode, "monitor") == 0) ? "MON" :
                        (strcmp(d->mode, "status") == 0)  ? "STAT" : "TXT";
    if (is_large) {
        char right_tag[28];
        snprintf(right_tag, sizeof(right_tag), "%s %dx%d", badge, c->cw, c->ch);
        canvas_draw_str_right(c, c->cw - pad, (hdr_h - 16) / 2, right_tag, EPD_WHITE, 1);
    } else if (is_wide) {
        canvas_draw_str_right(c, c->cw - pad, hdr_y, badge, EPD_WHITE, 1);
    }

    system_caps_t sc;
    system_caps_get(&sc);
    char up_buf[20];
    format_uptime(sc.uptime_sec, up_buf, sizeof(up_buf));

    int body_top = hdr_h + (is_large ? 10 : 4);
    int body_bot = c->ch - ftr_h - 2;

    if (strcmp(d->mode, "text") == 0) {
        int scale = is_large ? 2 : 1;
        int line_h = is_large ? 38 : (is_wide ? 17 : 31);
        for (int i = 0; i < APP_DISPLAY_MAX_LINES; i++) {
            int y = body_top + i * line_h;
            if (y + 16 * scale > body_bot) break;

            bool inline_red = false;
            const char *txt = strip_red_tag(d->lines[i], &inline_red);
            if (!txt[0]) continue;

            bool mask_red = ((d->red_mask & (1U << i)) != 0);
            bool use_red = (inline_red || mask_red) && (strcmp(d->theme, "mono") != 0) && c->spec->has_red;
            epd_color_t fg = use_red ? EPD_RED : EPD_BLACK;

            if (use_red && is_large) {
                canvas_fill_rect(c, pad, y + 4, 4, 24, EPD_RED);
                canvas_draw_str(c, pad + 10, y, txt, fg, scale);
            } else {
                canvas_draw_str(c, pad, y, txt, fg, scale);
            }
            if (!is_wide && !is_large && (int)strlen(txt) > 14 && y + 32 <= body_bot) {
                canvas_draw_str(c, pad, y + 16, txt + 14, fg, 1);
            }
        }
    } else if (strcmp(d->mode, "status") == 0) {
        if (is_large) {
            // 400x300 rich node status dashboard
            char lbuf[128];
            canvas_draw_rect(c, pad, body_top, c->cw - 2 * pad, 86, EPD_BLACK);
            snprintf(lbuf, sizeof(lbuf), "IP: %s", sc.ip_addr[0] ? sc.ip_addr : "Disconnected");
            canvas_draw_str(c, pad + 10, body_top + 8, lbuf, EPD_RED, 2);

            snprintf(lbuf, sizeof(lbuf), "WiFi: %.20s (%ddBm) %.16s",
                     sc.wifi_ssid[0] ? sc.wifi_ssid : "-", sc.wifi_rssi, cfg->mdns_host);
            canvas_draw_str(c, pad + 10, body_top + 44, lbuf, EPD_BLACK, 1);

            snprintf(lbuf, sizeof(lbuf), "Chip: %.24s | %luMB/%luMB",
                     sc.chip_name, (unsigned long)sc.flash_size_mb, (unsigned long)(sc.total_psram / (1024 * 1024)));
            canvas_draw_str(c, pad + 10, body_top + 64, lbuf, EPD_BLACK, 1);

            int row2 = body_top + 96;
            canvas_draw_rect(c, pad, row2, (c->cw - 3 * pad) / 2, 74, EPD_BLACK);
            snprintf(lbuf, sizeof(lbuf), "HEAP %lu KB", (unsigned long)(sc.free_heap / 1024));
            canvas_draw_str(c, pad + 10, row2 + 8, lbuf, EPD_BLACK, 2);
            snprintf(lbuf, sizeof(lbuf), "PSRAM Free: %lu KB", (unsigned long)(sc.free_psram / 1024));
            canvas_draw_str(c, pad + 10, row2 + 46, lbuf, EPD_BLACK, 1);

            int col2_x = pad * 2 + (c->cw - 3 * pad) / 2;
            canvas_draw_rect(c, col2_x, row2, (c->cw - 3 * pad) / 2, 74, EPD_BLACK);
            snprintf(lbuf, sizeof(lbuf), "UP %s", up_buf);
            canvas_draw_str(c, col2_x + 10, row2 + 8, lbuf, EPD_BLACK, 2);
            snprintf(lbuf, sizeof(lbuf), "Refresh #%lu (%lums)",
                     (unsigned long)(s_refresh_count + 1), (unsigned long)s_last_refresh_ms);
            canvas_draw_str(c, col2_x + 10, row2 + 46, lbuf, EPD_BLACK, 1);

            if (d->lines[0][0]) {
                bool tag = false;
                const char *note = strip_red_tag(d->lines[0], &tag);
                epd_color_t ncol = (tag || (d->red_mask & 1)) ? EPD_RED : EPD_BLACK;
                canvas_draw_str(c, pad, row2 + 84, note, ncol, 2);
            }
        } else {
            // 2.13" portrait or landscape clean status list
            char rows[6][64];
            snprintf(rows[0], sizeof(rows[0]), "IP:%s", sc.ip_addr[0] ? sc.ip_addr : "-");
            snprintf(rows[1], sizeof(rows[1]), "WF:%.16s (%ddBm)", sc.wifi_ssid[0] ? sc.wifi_ssid : "-", sc.wifi_rssi);
            snprintf(rows[2], sizeof(rows[2]), "HEAP:%luK", (unsigned long)(sc.free_heap / 1024));
            snprintf(rows[3], sizeof(rows[3]), "PSRM:%luK UP:%s", (unsigned long)(sc.free_psram / 1024), up_buf);
            snprintf(rows[4], sizeof(rows[4]), "PNL:%s", s_specs[panel_id].id);
            strlcpy(rows[5], d->lines[0], sizeof(rows[5]));

            int step = is_wide ? 16 : 28;
            for (int i = 0; i < 6; i++) {
                int y = body_top + i * step;
                if (y + 16 > body_bot || !rows[i][0]) continue;
                epd_color_t fg = (i == 0 && c->spec->has_red) ? EPD_RED : EPD_BLACK;
                canvas_draw_str(c, pad, y, rows[i], fg, 1);
            }
        }
    } else { // "monitor" mode
        bool has_push = (s_metrics.updated_ms > 0) && (((esp_timer_get_time() / 1000LL) - s_metrics.updated_ms) < 120000);
        float cpu = has_push ? s_metrics.cpu : 0.0f;
        float ram = has_push ? s_metrics.ram :
            (sc.total_heap ? (100.0f * (float)(sc.total_heap - sc.free_heap) / (float)sc.total_heap) : 0.0f);
        float disk = has_push ? s_metrics.disk :
            (sc.total_psram ? (100.0f * (float)(sc.total_psram - sc.free_psram) / (float)sc.total_psram) : 0.0f);

        char lbuf[64];
        if (is_large) {
            int y = body_top;
            if (d->show_cpu) {
                snprintf(lbuf, sizeof(lbuf), "CPU %3d%%", (int)(cpu + 0.5f));
                canvas_draw_str(c, pad, y, lbuf, EPD_BLACK, 2);
                canvas_draw_progress(c, 160, y + 4, c->cw - 175, 22, cpu, cpu >= 80.0f ? EPD_RED : EPD_BLACK);
                y += 42;
            }
            if (d->show_ram) {
                snprintf(lbuf, sizeof(lbuf), "%s %3d%%", has_push ? "RAM" : "HP ", (int)(ram + 0.5f));
                canvas_draw_str(c, pad, y, lbuf, EPD_BLACK, 2);
                canvas_draw_progress(c, 160, y + 4, c->cw - 175, 22, ram, ram >= 85.0f ? EPD_RED : EPD_BLACK);
                y += 42;
            }
            if (d->show_disk) {
                snprintf(lbuf, sizeof(lbuf), "%s %3d%%", has_push ? "DSK" : "PS ", (int)(disk + 0.5f));
                canvas_draw_str(c, pad, y, lbuf, EPD_BLACK, 2);
                canvas_draw_progress(c, 160, y + 4, c->cw - 175, 22, disk, EPD_BLACK);
                y += 42;
            }
            if (d->show_net && y + 32 <= body_bot) {
                snprintf(lbuf, sizeof(lbuf), "D:%s  U:%s", s_metrics.dl, s_metrics.ul);
                canvas_draw_str(c, pad, y, lbuf, EPD_BLACK, 2);
                y += 38;
            }
            if (d->lines[0][0] && y + 32 <= body_bot) {
                bool tag = false;
                const char *note = strip_red_tag(d->lines[0], &tag);
                canvas_draw_str(c, pad, y, note, (tag || (d->red_mask & 1)) ? EPD_RED : EPD_BLACK, 2);
            }
        } else {
            int y = body_top;
            int step = is_wide ? 22 : 34;
            if (d->show_cpu && y + 16 <= body_bot) {
                snprintf(lbuf, sizeof(lbuf), "CPU:%d%%", (int)(cpu + 0.5f));
                canvas_draw_str(c, pad, y, lbuf, EPD_BLACK, 1);
                canvas_draw_progress(c, is_wide ? 78 : pad, is_wide ? y + 2 : y + 18,
                                     is_wide ? c->cw - 84 : c->cw - 2 * pad, 10, cpu, EPD_RED);
                y += step;
            }
            if (d->show_ram && y + 16 <= body_bot) {
                snprintf(lbuf, sizeof(lbuf), "%s:%d%%", has_push ? "RAM" : "HP", (int)(ram + 0.5f));
                canvas_draw_str(c, pad, y, lbuf, EPD_BLACK, 1);
                canvas_draw_progress(c, is_wide ? 78 : pad, is_wide ? y + 2 : y + 18,
                                     is_wide ? c->cw - 84 : c->cw - 2 * pad, 10, ram, EPD_BLACK);
                y += step;
            }
            if (d->show_net && y + 16 <= body_bot) {
                snprintf(lbuf, sizeof(lbuf), "IP:%s", sc.ip_addr[0] ? sc.ip_addr : "-");
                canvas_draw_str(c, pad, y, lbuf, EPD_BLACK, 1);
                y += 18;
            }
            if (d->lines[0][0] && y + 16 <= body_bot) {
                bool tag = false;
                const char *note = strip_red_tag(d->lines[0], &tag);
                canvas_draw_str(c, pad, y, note, (tag || (d->red_mask & 1)) ? EPD_RED : EPD_BLACK, 1);
            }
        }
    }

    // Footer bar
    int fy = c->ch - ftr_h;
    canvas_fill_rect(c, 0, fy, c->cw, 1, EPD_BLACK);
    epd_color_t ftr_col = (c->spec->has_red && (d->red_mask & 0x80)) ? EPD_RED : EPD_BLACK;
    char fbuf[96];
    if (d->footer_text[0]) {
        strlcpy(fbuf, d->footer_text, sizeof(fbuf));
    } else if (is_large) {
        snprintf(fbuf, sizeof(fbuf), "%s | %s | #%lu | UP %s",
                 s_specs[panel_id].id, sc.ip_addr[0] ? sc.ip_addr : "-",
                 (unsigned long)(s_refresh_count + 1), up_buf);
    } else {
        snprintf(fbuf, sizeof(fbuf), "%s %s", sc.ip_addr[0] ? sc.ip_addr : "-", up_buf);
    }
    canvas_draw_str(c, pad, fy + 4, fbuf, ftr_col, 1);
}

/* ==================== Background Async Worker Task ==================== */

static void epd_worker_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // Allow WiFi DHCP to settle before first boot paint
    while (true) {
        app_config_t cfg;
        app_config_get(&cfg);
        TickType_t wait_ticks = portMAX_DELAY;
        if (cfg.display.refresh_sec > 0 && strcmp(cfg.display.mode, "off") != 0) {
            hal_epd_panel_t p = hal_epd_parse_panel(cfg.display.panel);
            uint32_t min_sec = s_specs[p].supports_partial ? 15U : 30U;
            uint32_t eff_sec = (uint32_t)cfg.display.refresh_sec;
            if (eff_sec < min_sec) eff_sec = min_sec;
            wait_ticks = pdMS_TO_TICKS(eff_sec * 1000U);
        }

        uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
        (void)notified;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
            continue;
        }
        if (!s_initialized || !s_spi) {
            xSemaphoreGive(s_lock);
            continue;
        }

        do {
            bool do_full = s_pending_full;
            bool do_clear = s_pending_clear;
            s_pending_full = false;
            s_pending_clear = false;
            s_pending_redraw = false;

            save_dirty_image_to_storage();
            app_config_get(&cfg);
            if (strcmp(cfg.display.mode, "image") == 0 && !s_img_loaded) {
                load_image_from_storage_if_needed();
            }
            hal_epd_panel_t pid = hal_epd_parse_panel(cfg.display.panel);
            const epd_panel_spec_t *spec = &s_specs[pid];

            s_refreshing = true;
            epd_canvas_t cv;
            canvas_begin(&cv, pid, cfg.display.rotation, cfg.display.invert);
            if (!do_clear) {
                render_epd_frame(&cfg, &cv, pid);
            }

            bool panel_changed = !s_panel_painted || (s_last_painted_panel != pid);
            bool allow_partial = (!do_full && !do_clear && !panel_changed &&
                                  spec->supports_partial && cfg.display.partial_refresh &&
                                  (s_partial_streak < 9));
            if (allow_partial) {
                s_partial_streak++;
            }

            epd_flush_to_hardware(pid, allow_partial);
            s_refreshing = false;
        } while (s_pending_redraw && s_initialized && s_spi);

        xSemaphoreGive(s_lock);
    }
}

static esp_err_t setup_spi_and_pins(void)
{
    const app_config_t *cfg = app_config_ptr();
    resolve_pins(&cfg->display, &s_pin_mosi, &s_pin_clk, &s_pin_cs, &s_pin_dc, &s_pin_rst, &s_pin_busy);

    if (s_pin_mosi < 0 || s_pin_clk < 0 || s_pin_cs < 0 || s_pin_dc < 0) {
        ESP_LOGW(TAG, "EPD SPI pins invalid (MOSI=%d CLK=%d CS=%d DC=%d)",
                 s_pin_mosi, s_pin_clk, s_pin_cs, s_pin_dc);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t out_io = {
        .pin_bit_mask = (1ULL << s_pin_dc) | (s_pin_rst >= 0 ? (1ULL << s_pin_rst) : 0ULL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&out_io);
    if (s_pin_rst >= 0) gpio_set_level(s_pin_rst, 1);

    if (s_pin_busy >= 0) {
        gpio_config_t in_io = {
            .pin_bit_mask = (1ULL << s_pin_busy),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE, // Keep LOW if panel unconnected
        };
        gpio_config(&in_io);
    }

    spi_bus_config_t bus = {
        .mosi_io_num = s_pin_mosi,
        .miso_io_num = -1,
        .sclk_io_num = s_pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        s_bus_owned = false; // Bus already initialized elsewhere
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 bus init failed: %s", esp_err_to_name(err));
        return err;
    } else {
        s_bus_owned = true;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = s_pin_cs,
        .queue_size = 4,
    };
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 add device failed: %s", esp_err_to_name(err));
        if (s_bus_owned) {
            spi_bus_free(SPI2_HOST);
            s_bus_owned = false;
        }
        s_spi = NULL;
        return err;
    }
    return ESP_OK;
}

esp_err_t hal_epd_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_img_lock) s_img_lock = xSemaphoreCreateMutex();
    if (!s_bw_buf) {
        s_bw_buf = heap_caps_calloc(1, EPD_MAX_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!s_red_buf) {
        s_red_buf = heap_caps_calloc(1, EPD_MAX_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!s_prev_bw_buf) {
        s_prev_bw_buf = heap_caps_calloc(1, EPD_BW213_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!s_bw_buf || !s_red_buf) {
        ESP_LOGE(TAG, "Failed to allocate EPD DMA framebuffers");
        return ESP_ERR_NO_MEM;
    }

    load_image_from_storage_if_needed();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_initialized) {
        esp_err_t err = setup_spi_and_pins();
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            return err;
        }
        s_initialized = true;
        s_panel_painted = false;
        ESP_LOGI(TAG, "EPD HAL ready (MOSI=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d)",
                 s_pin_mosi, s_pin_clk, s_pin_cs, s_pin_dc, s_pin_rst, s_pin_busy);
    }
    xSemaphoreGive(s_lock);

    if (!s_worker) {
        xTaskCreate(epd_worker_task, "epd_worker", 6144, NULL, 4, &s_worker);
    }
    return ESP_OK;
}

esp_err_t hal_epd_reconfigure(void)
{
    if (!s_initialized) return hal_epd_init();

    const app_config_t *cfg = app_config_ptr();
    int nm, nc, ncs, ndc, nrst, nbusy;
    resolve_pins(&cfg->display, &nm, &nc, &ncs, &ndc, &nrst, &nbusy);
    if (nm == s_pin_mosi && nc == s_pin_clk && ncs == s_pin_cs &&
        ndc == s_pin_dc && nrst == s_pin_rst && nbusy == s_pin_busy) {
        return ESP_OK;
    }

    if (!s_lock) return hal_epd_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_bus_owned) {
        spi_bus_free(SPI2_HOST);
        s_bus_owned = false;
    }
    s_initialized = false;
    s_panel_painted = false;
    esp_err_t err = setup_spi_and_pins();
    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "EPD pins reconfigured (MOSI=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d)",
                 s_pin_mosi, s_pin_clk, s_pin_cs, s_pin_dc, s_pin_rst, s_pin_busy);
    }
    xSemaphoreGive(s_lock);
    return err;
}

void hal_epd_deinit(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_bus_owned) {
        spi_bus_free(SPI2_HOST);
        s_bus_owned = false;
    }
    s_initialized = false;
    s_panel_painted = false;
    xSemaphoreGive(s_lock);
}

esp_err_t hal_epd_request_refresh(bool force_full, bool clear_only)
{
    if (!s_initialized) {
        esp_err_t err = hal_epd_init();
        if (err != ESP_OK) return err;
    }
    if (force_full) s_pending_full = true;
    if (clear_only) s_pending_clear = true;
    s_pending_redraw = true;
    if (s_worker) {
        xTaskNotifyGive(s_worker);
    }
    return ESP_OK;
}

esp_err_t hal_epd_apply_image_json(const char *json_str, bool *out_force_full)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_img_lock) {
        esp_err_t init_err = hal_epd_init();
        if (init_err != ESP_OK && !s_img_lock) return init_err;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *jw = cJSON_GetObjectItem(root, "width");
    const cJSON *jh = cJSON_GetObjectItem(root, "height");
    const cJSON *jbw = cJSON_GetObjectItem(root, "bw_b64");
    const cJSON *jred = cJSON_GetObjectItem(root, "red_b64");
    const cJSON *jcm = cJSON_GetObjectItem(root, "color_mode");
    const cJSON *jenc = cJSON_GetObjectItem(root, "encoding");
    const cJSON *jff = cJSON_GetObjectItem(root, "full_refresh");

    int w = cJSON_IsNumber(jw) ? jw->valueint : 0;
    int h = cJSON_IsNumber(jh) ? jh->valueint : 0;
    if (w < 1 || w > 400 || h < 1 || h > 400 || !cJSON_IsString(jbw) || !jbw->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    size_t stride = ((size_t)w + 7U) / 8U;
    size_t plane_len = stride * (size_t)h;
    if (plane_len == 0 || plane_len > EPD_MAX_BUF_SIZE) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    bool use_rle = (cJSON_IsString(jenc) && strcmp(jenc->valuestring, "rle") == 0);
    bool want_red = (cJSON_IsString(jcm) && strcmp(jcm->valuestring, "bwr") == 0 &&
                     cJSON_IsString(jred) && jred->valuestring[0] != '\0');
    bool force_full = cJSON_IsBool(jff) ? cJSON_IsTrue(jff) : true;

    uint8_t *tmp_bw = calloc(1, EPD_MAX_BUF_SIZE);
    uint8_t *tmp_red = calloc(1, EPD_MAX_BUF_SIZE);
    if (!tmp_bw || !tmp_red) {
        free(tmp_bw);
        free(tmp_red);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    bool ok = decode_plane_b64(jbw->valuestring, use_rle, plane_len, tmp_bw);
    if (ok && want_red) {
        ok = decode_plane_b64(jred->valuestring, use_rle, plane_len, tmp_red);
    }
    cJSON_Delete(root);

    if (!ok) {
        free(tmp_bw);
        free(tmp_red);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_img_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        free(tmp_bw);
        free(tmp_red);
        return ESP_ERR_TIMEOUT;
    }
    bool alloc_ok = ensure_img_buffers_locked();
    if (alloc_ok) {
        memset(s_img_bw, 0xFF, EPD_MAX_BUF_SIZE);
        memset(s_img_red, 0x00, EPD_MAX_BUF_SIZE);
        memcpy(s_img_bw, tmp_bw, plane_len);
        if (want_red) {
            memcpy(s_img_red, tmp_red, plane_len);
        }
        s_img_w = (uint16_t)w;
        s_img_h = (uint16_t)h;
        s_img_has_red = want_red;
        s_img_loaded = true;
        s_img_dirty = true;
    }
    xSemaphoreGive(s_img_lock);
    free(tmp_bw);
    free(tmp_red);

    if (!alloc_ok) return ESP_ERR_NO_MEM;

    app_config_t updated;
    app_config_get(&updated);
    app_config_apply_display_json(json_str, &updated, false);
    strlcpy(updated.display.mode, "image", sizeof(updated.display.mode));
    updated.config_ver++;
    app_config_save(&updated);

    if (out_force_full) *out_force_full = force_full;
    ESP_LOGI(TAG, "Queued EPD image %dx%d (%s, %u B/plane)", w, h, want_red ? "bwr" : "bw", (unsigned)plane_len);
    return ESP_OK;
}

void hal_epd_update_metrics(float cpu, float ram, float disk, const char *dl, const char *ul, const char *uptime, const char *time_str)
{
    s_metrics.cpu = cpu;
    s_metrics.ram = ram;
    s_metrics.disk = disk;
    if (dl) strlcpy(s_metrics.dl, dl, sizeof(s_metrics.dl));
    if (ul) strlcpy(s_metrics.ul, ul, sizeof(s_metrics.ul));
    if (uptime) strlcpy(s_metrics.uptime, uptime, sizeof(s_metrics.uptime));
    if (time_str) strlcpy(s_metrics.time_str, time_str, sizeof(s_metrics.time_str));
    s_metrics.updated_ms = esp_timer_get_time() / 1000LL;
}

void hal_epd_get_info(hal_epd_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const app_config_t *cfg = app_config_ptr();
    hal_epd_panel_t pid = hal_epd_parse_panel(cfg->display.panel);
    const epd_panel_spec_t *spec = &s_specs[pid];
    uint16_t rot = (cfg->display.rotation == 90 || cfg->display.rotation == 180 || cfg->display.rotation == 270)
                 ? cfg->display.rotation : 0;

    out->initialized = s_initialized;
    out->refreshing = s_refreshing;
    out->busy_timeout = s_busy_timeout;
    out->has_red = spec->has_red;
    out->supports_partial = spec->supports_partial;
    out->has_image = (s_img_bw != NULL && s_img_w > 0 && s_img_h > 0);
    out->image_has_red = s_img_has_red;
    out->image_w = s_img_w;
    out->image_h = s_img_h;
    out->panel = pid;
    out->panel_id = spec->id;
    out->driver_name = spec->driver;
    out->native_w = spec->vis_w;
    out->native_h = spec->vis_h;
    out->width = (rot == 90 || rot == 270) ? spec->vis_h : spec->vis_w;
    out->height = (rot == 90 || rot == 270) ? spec->vis_w : spec->vis_h;
    out->refresh_count = s_refresh_count;
    out->last_refresh_ms = s_last_refresh_ms;
    resolve_pins(&cfg->display, &out->pin_mosi, &out->pin_clk, &out->pin_cs, &out->pin_dc, &out->pin_rst, &out->pin_busy);
}

#else

bool hal_epd_is_epd_panel_name(const char *panel_name) { (void)panel_name; return false; }
hal_epd_panel_t hal_epd_parse_panel(const char *panel_name) { (void)panel_name; return HAL_EPD_PANEL_213_BWR; }
const char *hal_epd_panel_name(hal_epd_panel_t panel) { (void)panel; return "epd_213_bwr"; }
esp_err_t hal_epd_init(void) { return ESP_OK; }
esp_err_t hal_epd_reconfigure(void) { return ESP_OK; }
void hal_epd_deinit(void) {}
esp_err_t hal_epd_request_refresh(bool force_full, bool clear_only) { (void)force_full; (void)clear_only; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t hal_epd_apply_image_json(const char *json_str, bool *out_force_full)
{
    (void)json_str; (void)out_force_full;
    return ESP_ERR_NOT_SUPPORTED;
}
void hal_epd_update_metrics(float cpu, float ram, float disk, const char *dl, const char *ul, const char *uptime, const char *time_str)
{
    (void)cpu; (void)ram; (void)disk; (void)dl; (void)ul; (void)uptime; (void)time_str;
}
void hal_epd_get_info(hal_epd_info_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

#endif
