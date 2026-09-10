#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise NimBLE host, advertise as a HID keyboard+mouse with name `device_name`. */
esp_err_t hid_ble_init(const char *device_name);

/** True after a host has connected and bonded. */
bool hid_ble_connected(void);

/** Press + release a key (with optional modifier byte). */
void hid_ble_tap(uint8_t keycode, uint8_t modifiers);

/** Press a key (sets it in the report; remains until hid_ble_up). */
void hid_ble_down(uint8_t keycode, uint8_t modifiers);

/** Release a key. */
void hid_ble_up(uint8_t keycode, uint8_t modifiers);

/** Relative mouse move on the BLE HID mouse report. */
void hid_ble_mouse_move(int8_t dx, int8_t dy);

/** Press + release a mouse button bitmask: 1=left, 2=right, 4=middle. */
void hid_ble_mouse_click(uint8_t button);

/** Relative wheel / pan scroll deltas. */
void hid_ble_mouse_scroll(int8_t vertical, int8_t horizontal);

/* ---------- multi-host management (PR4) ---------- */

typedef struct {
    uint8_t addr_type;   /* NimBLE addr type */
    uint8_t addr[6];     /* peer device address, little-endian (NimBLE order) */
} hid_ble_peer_t;

/** Fill out[] with up to max bonded peers. Returns count, or <0 on error. */
int  hid_ble_list_bonds(hid_ble_peer_t *out, int max);

/** Get the currently-connected peer's address. False when not connected. */
bool hid_ble_get_current_peer(hid_ble_peer_t *out);

/** Erase a single bonded peer from the keystore. Returns 0 ok, !=0 NimBLE rc. */
int  hid_ble_unpair(uint8_t addr_type, const uint8_t addr[6]);

/** Erase all bonded peers + restart advertising. */
int  hid_ble_unpair_all(void);

/** Drop the current GAP link (host can reconnect). */
int  hid_ble_disconnect(void);

/** Disconnect (if connected) and start directed advertising to the given peer. */
int  hid_ble_switch_to(uint8_t addr_type, const uint8_t addr[6]);

/* ---------- BLE scan (Observer role) ---------- */

#define HID_BLE_SCAN_MAX 16

typedef struct {
    uint8_t addr_type;
    uint8_t addr[6];
    int8_t  rssi;
    char    name[32];   /* parsed from adv complete/short name; empty if absent */
} hid_ble_scan_entry_t;

/** Start a synchronous BLE scan for `duration_ms` (capped at 10000), collect up to
 *  `max` unique advertisers into `out`, and resume HID advertising. Returns the
 *  count actually written, or <0 on error. */
int  hid_ble_scan(int duration_ms, hid_ble_scan_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
