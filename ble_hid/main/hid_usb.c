/*
 * USB HID transport (TinyUSB-based composite Keyboard + Mouse).
 *
 * Coexists with hid_ble.c — both transports route HID reports independently.
 * Builds only on chips with native USB-OTG (ESP32-S2 / S3 / P4); on other
 * targets the API stubs out and hid_usb_init() returns ESP_ERR_NOT_SUPPORTED.
 */
#include "hid_usb.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#define HID_USB_HW 1
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#if CONFIG_TINYUSB_CDC_ENABLED
#include "class/cdc/cdc.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#endif
#else
#define HID_USB_HW 0
#endif

static const char *TAG = "hid_usb";

#if HID_USB_HW

#define RID_KBD   1
#define RID_MOUSE 2

static const uint8_t REPORT_DESC[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KBD)),
    TUD_HID_REPORT_DESC_MOUSE   (HID_REPORT_ID(RID_MOUSE)),
};

static const tusb_desc_device_t DEV_DESC = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
#if CONFIG_TINYUSB_CDC_ENABLED
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
#endif
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,  /* Espressif */
    .idProduct          = 0x400C,  /* Composite HID + CDC */
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *STRING_DESC[] = {
    (char[]){0x09, 0x04},     /* 0: langid: en-us */
    "Espressif",              /* 1: manufacturer */
    "ESP32 Net HID",          /* 2: product       */
    "00000001",               /* 3: serial        */
    "ESP32 HID Interface",    /* 4: HID interface */
#if CONFIG_TINYUSB_CDC_ENABLED
    "ESP32 CDC Console",      /* 5: CDC interface */
#endif
};

enum {
    ITF_NUM_HID = 0,
#if CONFIG_TINYUSB_CDC_ENABLED
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
#endif
    ITF_NUM_TOTAL
};

#define EPNUM_HID       0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_DATA  0x03

#if CONFIG_TINYUSB_CDC_ENABLED
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#endif

static const uint8_t CFG_DESC[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, false, sizeof(REPORT_DESC), EPNUM_HID, 16, 10),
#if CONFIG_TINYUSB_CDC_ENABLED
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_DATA, (0x80 | EPNUM_CDC_DATA), 64),
#endif
};

static volatile bool s_mounted = false;
static char          s_dev_name[32];
static uint8_t       s_mouse_buttons = 0;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return REPORT_DESC; }

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{ (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen; return 0; }

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{ (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize; }

void tud_mount_cb  (void) { s_mounted = true;  ESP_LOGI(TAG, "USB mounted"); }
void tud_umount_cb (void) { s_mounted = false; s_mouse_buttons = 0; ESP_LOGW(TAG, "USB unmounted"); }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; s_mounted = false; }
void tud_resume_cb (void)                  { s_mounted = true; }

static void send_kbd(uint8_t mods, uint8_t kc)
{
    if (!s_mounted) return;
    uint8_t keys[6] = { kc, 0, 0, 0, 0, 0 };
    tud_hid_keyboard_report(RID_KBD, mods, kc ? keys : NULL);
}

bool hid_usb_mounted(void) { return s_mounted; }

void hid_usb_tap(uint8_t code, uint8_t mods)
{
    send_kbd(mods, code);
    vTaskDelay(pdMS_TO_TICKS(8));
    send_kbd(0, 0);
}
void hid_usb_down(uint8_t code, uint8_t mods) { send_kbd(mods, code); }
void hid_usb_up  (uint8_t code, uint8_t mods) { (void)code; (void)mods; send_kbd(0, 0); }

void hid_usb_mouse_move(int8_t dx, int8_t dy)
{
    if (!s_mounted) return;
    tud_hid_mouse_report(RID_MOUSE, s_mouse_buttons, dx, dy, 0, 0);
}
void hid_usb_mouse_down(uint8_t button)
{
    if (!s_mounted) return;
    s_mouse_buttons |= (uint8_t)(button & 0x07);
    tud_hid_mouse_report(RID_MOUSE, s_mouse_buttons, 0, 0, 0, 0);
}
void hid_usb_mouse_up(uint8_t button)
{
    if (!s_mounted) return;
    s_mouse_buttons &= (uint8_t)~(button & 0x07);
    tud_hid_mouse_report(RID_MOUSE, s_mouse_buttons, 0, 0, 0, 0);
}
void hid_usb_mouse_click(uint8_t button)
{
    if (!s_mounted) return;
    hid_usb_mouse_down(button);
    vTaskDelay(pdMS_TO_TICKS(8));
    hid_usb_mouse_up(button);
}
void hid_usb_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    if (!s_mounted) return;
    tud_hid_mouse_report(RID_MOUSE, s_mouse_buttons, 0, 0, vertical, horizontal);
}

esp_err_t hid_usb_init(const char *device_name)
{
    strlcpy(s_dev_name, device_name ? device_name : "esp32-hid", sizeof s_dev_name);
    STRING_DESC[2] = s_dev_name;

    const tinyusb_config_t cfg = {
        .device_descriptor        = &DEV_DESC,
        .string_descriptor        = STRING_DESC,
        .string_descriptor_count  = sizeof(STRING_DESC) / sizeof(STRING_DESC[0]),
        .external_phy             = false,
        .configuration_descriptor = CFG_DESC,
    };
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return err;
    }

#if CONFIG_TINYUSB_CDC_ENABLED
    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    esp_err_t cdc_err = tusb_cdc_acm_init(&acm_cfg);
    if (cdc_err == ESP_OK) {
        esp_tusb_init_console(TINYUSB_CDC_ACM_0);
        ESP_LOGI(TAG, "USB CDC ACM Console enabled on TINYUSB_CDC_ACM_0");
    } else {
        ESP_LOGW(TAG, "tusb_cdc_acm_init failed: %s", esp_err_to_name(cdc_err));
    }
#endif

    ESP_LOGI(TAG, "USB HID composite up (HID + CDC)");
    return ESP_OK;
}

#else  /* !HID_USB_HW */

esp_err_t hid_usb_init(const char *device_name) { (void)device_name; return ESP_ERR_NOT_SUPPORTED; }
bool      hid_usb_mounted(void)                 { return false; }
void      hid_usb_tap (uint8_t c, uint8_t m)    { (void)c; (void)m; }
void      hid_usb_down(uint8_t c, uint8_t m)    { (void)c; (void)m; }
void      hid_usb_up  (uint8_t c, uint8_t m)    { (void)c; (void)m; }
void hid_usb_mouse_move (int8_t x, int8_t y) { (void)x; (void)y; }
void hid_usb_mouse_click(uint8_t b)          { (void)b; }
void hid_usb_mouse_scroll(int8_t v, int8_t h) { (void)v; (void)h; }
void hid_usb_mouse_down (uint8_t b)          { (void)b; }
void hid_usb_mouse_up   (uint8_t b)          { (void)b; }

#endif
