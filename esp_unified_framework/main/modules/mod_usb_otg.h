#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mod_usb_otg_init(void);
esp_err_t mod_usb_otg_register_routes(httpd_handle_t server);
bool mod_usb_otg_is_mounted(void);

bool mod_usb_otg_type_text(const char *text, int interval_ms);
bool mod_usb_otg_key_tap(const char *key_name, uint8_t modifiers);
bool mod_usb_otg_key_down(const char *key_name, uint8_t modifiers);
bool mod_usb_otg_key_up(const char *key_name);
bool mod_usb_otg_mouse_move(int8_t dx, int8_t dy);
bool mod_usb_otg_mouse_scroll(int8_t vertical, int8_t horizontal);
bool mod_usb_otg_mouse_click(uint8_t button);
bool mod_usb_otg_mouse_btn(uint8_t button, bool down);

#ifdef __cplusplus
}
#endif
