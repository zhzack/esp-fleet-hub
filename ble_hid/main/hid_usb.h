#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise USB HID composite (keyboard + mouse) via TinyUSB.
 *  Safe to call once at boot. Calling on chips without USB-OTG is a no-op
 *  and returns ESP_ERR_NOT_SUPPORTED. */
esp_err_t hid_usb_init(const char *device_name);

/** True after the USB host has enumerated us as a HID device. */
bool hid_usb_mounted(void);

/* Keyboard */
void hid_usb_tap (uint8_t keycode, uint8_t modifiers);
void hid_usb_down(uint8_t keycode, uint8_t modifiers);
void hid_usb_up  (uint8_t keycode, uint8_t modifiers);

/* Mouse — dx/dy in [-127, 127], button bitmask: 1=left, 2=right, 4=middle */
void hid_usb_mouse_move (int8_t dx, int8_t dy);
void hid_usb_mouse_click(uint8_t button);
void hid_usb_mouse_scroll(int8_t vertical, int8_t horizontal);

#ifdef __cplusplus
}
#endif
