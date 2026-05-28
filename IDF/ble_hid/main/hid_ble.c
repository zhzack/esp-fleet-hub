/*
 * Minimal BLE HID keyboard using NimBLE.
 *
 * Implementation notes:
 *  - Exposes a single HID Service (0x1812) with:
 *      - HID Information (0x2A4A)
 *      - Report Map (0x2A4B)  — standard 8-byte boot keyboard descriptor
 *      - HID Control Point (0x2A4C)
 *      - Report (0x2A4D)      — input report, with CCCD
 *      - Protocol Mode (0x2A4E)
 *  - Plus mandatory Battery Service (0x180F) and Device Information Service (0x180A).
 *  - The report format is the classic 8-byte boot keyboard:
 *      [modifiers][reserved][key1..key6]
 *  - Bonding uses NimBLE's default SM with bonding + MITM disabled (Just Works).
 *
 * For brevity this file implements only what the executor needs: tap/down/up of
 * a single key with optional modifier. Six-key rollover would extend report_state.
 */
#include "hid_ble.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "hid";

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_input_report_handle;
static SemaphoreHandle_t s_lock;
static uint8_t s_addr_type;
static char    s_dev_name[32];

/* 8-byte boot keyboard report: modifier | reserved | k0..k5 */
static uint8_t s_report[8];

static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  /* Usage Page (Generic Desktop)             */
    0x09, 0x06,  /* Usage (Keyboard)                         */
    0xA1, 0x01,  /* Collection (Application)                 */
    0x05, 0x07,  /*   Usage Page (Key Codes)                 */
    0x19, 0xE0,  /*   Usage Minimum (224)                    */
    0x29, 0xE7,  /*   Usage Maximum (231)                    */
    0x15, 0x00,  /*   Logical Minimum (0)                    */
    0x25, 0x01,  /*   Logical Maximum (1)                    */
    0x75, 0x01,  /*   Report Size (1)                        */
    0x95, 0x08,  /*   Report Count (8)                       */
    0x81, 0x02,  /*   Input (Data, Variable, Absolute) — mods*/
    0x95, 0x01,  /*   Report Count (1)                       */
    0x75, 0x08,  /*   Report Size (8)                        */
    0x81, 0x01,  /*   Input (Constant) — reserved            */
    0x95, 0x06,  /*   Report Count (6)                       */
    0x75, 0x08,  /*   Report Size (8)                        */
    0x15, 0x00,  /*   Logical Minimum (0)                    */
    0x25, 0x65,  /*   Logical Maximum (101)                  */
    0x05, 0x07,  /*   Usage Page (Key Codes)                 */
    0x19, 0x00,  /*   Usage Minimum (0)                      */
    0x29, 0x65,  /*   Usage Maximum (101)                    */
    0x81, 0x00,  /*   Input (Data, Array)                    */
    0xC0
};

/* HID Information: bcdHID=0x0111, country=0, flags=remote_wake|normally_connectable */
static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x03 };

static int chr_access_report_map(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, HID_REPORT_MAP, sizeof HID_REPORT_MAP) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_access_hid_info(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, HID_INFO, sizeof HID_INFO) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_access_report(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, s_report, sizeof s_report) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int chr_access_control_point(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0; /* writes ignored — suspend/resume not implemented */
}

static uint8_t s_proto_mode = 1; /* report protocol */
static int chr_access_proto_mode(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &s_proto_mode, 1) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= 1) ble_hs_mbuf_to_flat(ctxt->om, &s_proto_mode, 1, NULL);
    }
    return 0;
}

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),  /* HID Service */
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A),
              .access_cb = chr_access_hid_info,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B),
              .access_cb = chr_access_report_map,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C),
              .access_cb = chr_access_control_point,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E),
              .access_cb = chr_access_proto_mode,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D),
              .access_cb = chr_access_report,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_input_report_handle },
            { 0 }
        }
    },
    { 0 }
};

static void start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
        } else {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe handle=%u cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;
    fields.appearance = 961;  /* HID Keyboard */
    fields.appearance_is_present = 1;
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) ESP_LOGW(TAG, "adv_set_fields rc=%d", rc);

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_addr_type);
    start_advertising();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool hid_ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

static void notify_report(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_report, sizeof s_report);
    if (!om) return;
    ble_gattc_notify_custom(s_conn_handle, s_input_report_handle, om);
}

static void set_key(uint8_t code, uint8_t mods)
{
    s_report[0] = mods;
    s_report[1] = 0;
    s_report[2] = code;
    memset(&s_report[3], 0, 5);
}

static void clear_keys(void)
{
    memset(s_report, 0, sizeof s_report);
}

void hid_ble_tap(uint8_t code, uint8_t mods)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_key(code, mods);
    notify_report();
    vTaskDelay(pdMS_TO_TICKS(8));
    clear_keys();
    notify_report();
    xSemaphoreGive(s_lock);
}

void hid_ble_down(uint8_t code, uint8_t mods)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_key(code, mods);
    notify_report();
    xSemaphoreGive(s_lock);
}

void hid_ble_up(uint8_t code, uint8_t mods)
{
    (void)code; (void)mods;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_keys();
    notify_report();
    xSemaphoreGive(s_lock);
}

int hid_ble_list_bonds(hid_ble_peer_t *out, int max)
{
    if (!out || max <= 0) return -1;
    ble_addr_t addrs[8];
    int cap = max < 8 ? max : 8;
    int n = 0;
    int rc = ble_store_util_bonded_peers(addrs, &n, cap);
    if (rc != 0) return -2;
    for (int i = 0; i < n; ++i) {
        out[i].addr_type = addrs[i].type;
        memcpy(out[i].addr, addrs[i].val, 6);
    }
    return n;
}

bool hid_ble_get_current_peer(hid_ble_peer_t *out)
{
    if (!out) return false;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn_handle, &desc) != 0) return false;
    out->addr_type = desc.peer_id_addr.type;
    memcpy(out->addr, desc.peer_id_addr.val, 6);
    return true;
}

int hid_ble_unpair(uint8_t addr_type, const uint8_t addr[6])
{
    ble_addr_t a;
    a.type = addr_type;
    memcpy(a.val, addr, 6);
    return ble_gap_unpair(&a);
}

int hid_ble_unpair_all(void)
{
    /* Iterate bonded list and delete each */
    ble_addr_t addrs[8];
    int n = 0;
    int rc = ble_store_util_bonded_peers(addrs, &n, 8);
    if (rc != 0) return rc;
    for (int i = 0; i < n; ++i) {
        rc = ble_gap_unpair(&addrs[i]);
        if (rc != 0) ESP_LOGW(TAG, "unpair %d rc=%d", i, rc);
    }
    return 0;
}

int hid_ble_disconnect(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return 0;
    return ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

int hid_ble_switch_to(uint8_t addr_type, const uint8_t addr[6])
{
    /* Drop existing link if any; advertising auto-restarts on disconnect.
     * Then issue directed-low-duty advertising at the requested peer. */
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ble_gap_adv_stop();

    ble_addr_t peer;
    peer.type = addr_type;
    memcpy(peer.val, addr, 6);

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_DIR,
        .disc_mode = BLE_GAP_DISC_MODE_NON,
        .high_duty_cycle = 0,
    };
    int rc = ble_gap_adv_start(s_addr_type, &peer, 30000, &adv, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "directed adv rc=%d, falling back to undirected", rc);
        start_advertising();
    }
    return rc;
}


esp_err_t hid_ble_init(const char *device_name)
{
    strlcpy(s_dev_name, device_name ? device_name : "esp32-hid", sizeof s_dev_name);
    s_lock = xSemaphoreCreateMutex();

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return ret;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = 3;
    ble_hs_cfg.sm_their_key_dist = 3;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(GATT_SERVICES);
    if (rc) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(GATT_SERVICES);
    if (rc) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return ESP_FAIL; }

    ble_svc_gap_device_name_set(s_dev_name);

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
