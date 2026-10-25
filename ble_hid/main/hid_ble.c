/*
 * Minimal BLE HID keyboard + mouse using NimBLE.
 *
 * Exposes one HID service with:
 *  - HID Information
 *  - Report Map
 *  - HID Control Point
 *  - Protocol Mode
 *  - Keyboard input report
 *  - Mouse input report
 *
 * Keyboard report format: [modifiers][reserved][key1..key6]
 * Mouse report format:    [buttons][x][y][wheel][pan]
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
#include "store/config/ble_store_config.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "hid";

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_keyboard_input_report_handle;
static uint16_t s_mouse_input_report_handle;
static SemaphoreHandle_t s_lock;
static uint8_t s_addr_type;
static char s_dev_name[32];
static volatile bool s_advertising = false;
static volatile bool s_scanning = false;
static volatile TickType_t s_adv_retry_at = 0;
static volatile bool s_adv_fields_ready = false;

static uint8_t s_keyboard_report[8];
static uint8_t s_mouse_report[5];
static uint8_t s_mouse_buttons = 0;

#define HID_REPORT_ID_KEYBOARD 1
#define HID_REPORT_ID_MOUSE 2
#define HID_REPORT_TYPE_INPUT 1

typedef struct {
    uint8_t *data;
    uint8_t len;
} hid_report_view_t;

static hid_report_view_t s_keyboard_report_view = {
    .data = s_keyboard_report,
    .len = sizeof(s_keyboard_report),
};

static hid_report_view_t s_mouse_report_view = {
    .data = s_mouse_report,
    .len = sizeof(s_mouse_report),
};

static uint8_t s_keyboard_report_ref[2] = {
    HID_REPORT_ID_KEYBOARD,
    HID_REPORT_TYPE_INPUT,
};

static uint8_t s_mouse_report_ref[2] = {
    HID_REPORT_ID_MOUSE,
    HID_REPORT_TYPE_INPUT,
};

static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,

    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, HID_REPORT_ID_MOUSE,
    0x09, 0x01,
    0xA1, 0x00,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,
    0x05, 0x0C,
    0x0A, 0x38, 0x02,
    0x95, 0x01,
    0x81, 0x06,
    0xC0,
    0xC0,
};

static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x03 };

static int chr_access_report_map(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, HID_REPORT_MAP, sizeof(HID_REPORT_MAP)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_access_hid_info(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, HID_INFO, sizeof(HID_INFO)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_access_input_report(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    hid_report_view_t *report = arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, report->data, report->len) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int chr_access_report_ref(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t *report_ref = arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        return os_mbuf_append(ctxt->om, report_ref, 2) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int chr_access_control_point(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

static uint8_t s_proto_mode = 1;

static int chr_access_proto_mode(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &s_proto_mode, 1) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &s_proto_mode, 1, NULL);
        }
    }
    return 0;
}

static struct ble_gatt_dsc_def KEYBOARD_REPORT_DESCRIPTORS[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),
        .att_flags = BLE_ATT_F_READ,
        .access_cb = chr_access_report_ref,
        .arg = s_keyboard_report_ref,
    },
    { 0 }
};

static struct ble_gatt_dsc_def MOUSE_REPORT_DESCRIPTORS[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),
        .att_flags = BLE_ATT_F_READ,
        .access_cb = chr_access_report_ref,
        .arg = s_mouse_report_ref,
    },
    { 0 }
};

static struct ble_gatt_chr_def HID_CHARACTERISTICS[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4A),
        .access_cb = chr_access_hid_info,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4B),
        .access_cb = chr_access_report_map,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4C),
        .access_cb = chr_access_control_point,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4E),
        .access_cb = chr_access_proto_mode,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4D),
        .access_cb = chr_access_input_report,
        .arg = &s_keyboard_report_view,
        .descriptors = KEYBOARD_REPORT_DESCRIPTORS,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_keyboard_input_report_handle,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4D),
        .access_cb = chr_access_input_report,
        .arg = &s_mouse_report_view,
        .descriptors = MOUSE_REPORT_DESCRIPTORS,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_mouse_input_report_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = HID_CHARACTERISTICS,
    },
    { 0 }
};

static void start_advertising(void);
static void schedule_advertising_retry(uint32_t delay_ms);
static int ensure_advertising_fields(void);

/* NimBLE config store 的入口函数（IDF 移植未纳入公共头文件，参照官方
 * bleprph 示例前置声明）：挂载 NVS 持久化 store 回调并装载历史配对密钥 */
void ble_store_config_init(void);

/* CCCD（通知订阅描述符）的全局 ATT 权限接口，NimBLE 私有 API：
 * 默认只有 READ|WRITE（无加密要求），macOS 的 HOGP 实现遇到"无需配对即可
 * 订阅"的 HID 设备会拒绝建键并陷入重连循环，必须强制加密触发 SMP 配对 */
void ble_gatts_set_clt_cfg_perm_flags(uint8_t flags);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_advertising = false;
            s_adv_retry_at = 0;
            ESP_LOGI(TAG, "connected");
        } else {
            s_advertising = false;
            schedule_advertising_retry(150);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_mouse_buttons = 0;
        s_advertising = false;
        schedule_advertising_retry(150);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete reason=0x%x", event->adv_complete.reason);
        s_advertising = false;
        schedule_advertising_retry(150);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe handle=%u cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* 对端保留了旧配对密钥而本机密钥库已丢失（如重启/OTA 后）时，
         * 对端重连加密会失败并反复重试；删除旧 bond 记录允许重新配对，
         * 配对密钥现由 ble_store_config 持久化到 NVS，正常重启不再丢。 */
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ESP_LOGW(TAG, "repeat pairing from peer — deleting stale bond");
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void schedule_advertising_retry(uint32_t delay_ms)
{
    TickType_t when = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    if (s_adv_retry_at == 0 || (int32_t)(when - s_adv_retry_at) < 0) {
        s_adv_retry_at = when;
    }
}

static int ensure_advertising_fields(void)
{
    if (s_adv_fields_ready) return 0;

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;
    fields.appearance = 960;
    fields.appearance_is_present = 1;
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc == 0) {
        s_adv_fields_ready = true;
    }
    return rc;
}

static void start_advertising(void)
{
    if (s_advertising) return;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
    if (s_scanning) return;
    if (s_adv_retry_at != 0) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(s_adv_retry_at - now) > 0) return;
        s_adv_retry_at = 0;
    }

    int rc = ensure_advertising_fields();
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields rc=%d", rc);
        schedule_advertising_retry(200);
        return;
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc == 0) {
        s_advertising = true;
        s_adv_retry_at = 0;
    } else if (rc == BLE_HS_EALREADY) {
        s_advertising = true;
        s_adv_retry_at = 0;
    } else if (rc == BLE_HS_ENOMEM) {
        ESP_LOGW(TAG, "adv_start deferred rc=%d; retrying shortly", rc);
        schedule_advertising_retry(150);
    } else {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_addr_type);
    s_adv_fields_ready = false;
    start_advertising();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void adv_watchdog_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE && !s_advertising && !s_scanning) {
            start_advertising();
        }
    }
}

bool hid_ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

static void notify_input_report(uint16_t handle, const uint8_t *report, uint8_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify handle=%u rc=%d", handle, rc);
}

static void set_key(uint8_t code, uint8_t mods)
{
    s_keyboard_report[0] = mods;
    s_keyboard_report[1] = 0;
    s_keyboard_report[2] = code;
    memset(&s_keyboard_report[3], 0, 5);
}

static void clear_keys(void)
{
    memset(s_keyboard_report, 0, sizeof(s_keyboard_report));
}

static void set_mouse_report(uint8_t buttons, int8_t dx, int8_t dy,
                             int8_t wheel, int8_t pan)
{
    s_mouse_report[0] = buttons & 0x07;
    s_mouse_report[1] = (uint8_t)dx;
    s_mouse_report[2] = (uint8_t)dy;
    s_mouse_report[3] = (uint8_t)wheel;
    s_mouse_report[4] = (uint8_t)pan;
}

static void clear_mouse_report(void)
{
    memset(s_mouse_report, 0, sizeof(s_mouse_report));
}

void hid_ble_tap(uint8_t code, uint8_t mods)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_key(code, mods);
    notify_input_report(s_keyboard_input_report_handle, s_keyboard_report,
                        sizeof(s_keyboard_report));
    vTaskDelay(pdMS_TO_TICKS(8));
    clear_keys();
    notify_input_report(s_keyboard_input_report_handle, s_keyboard_report,
                        sizeof(s_keyboard_report));
    xSemaphoreGive(s_lock);
}

void hid_ble_down(uint8_t code, uint8_t mods)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_key(code, mods);
    notify_input_report(s_keyboard_input_report_handle, s_keyboard_report,
                        sizeof(s_keyboard_report));
    xSemaphoreGive(s_lock);
}

void hid_ble_up(uint8_t code, uint8_t mods)
{
    (void)code;
    (void)mods;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_keys();
    notify_input_report(s_keyboard_input_report_handle, s_keyboard_report,
                        sizeof(s_keyboard_report));
    xSemaphoreGive(s_lock);
}

void hid_ble_mouse_move(int8_t dx, int8_t dy)
{
    if (dx == 0 && dy == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_mouse_report(s_mouse_buttons, dx, dy, 0, 0);
    notify_input_report(s_mouse_input_report_handle, s_mouse_report,
                        sizeof(s_mouse_report));
    clear_mouse_report();
    xSemaphoreGive(s_lock);
}

void hid_ble_mouse_down(uint8_t button)
{
    button &= 0x07;
    if (button == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mouse_buttons |= button;
    set_mouse_report(s_mouse_buttons, 0, 0, 0, 0);
    notify_input_report(s_mouse_input_report_handle, s_mouse_report,
                        sizeof(s_mouse_report));
    xSemaphoreGive(s_lock);
}

void hid_ble_mouse_up(uint8_t button)
{
    button &= 0x07;
    if (button == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mouse_buttons &= (uint8_t)~button;
    set_mouse_report(s_mouse_buttons, 0, 0, 0, 0);
    notify_input_report(s_mouse_input_report_handle, s_mouse_report,
                        sizeof(s_mouse_report));
    xSemaphoreGive(s_lock);
}

void hid_ble_mouse_click(uint8_t button)
{
    if ((button & 0x07) == 0) return;

    hid_ble_mouse_down(button);
    vTaskDelay(pdMS_TO_TICKS(8));
    hid_ble_mouse_up(button);
}

void hid_ble_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    if (vertical == 0 && horizontal == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_mouse_report(s_mouse_buttons, 0, 0, vertical, horizontal);
    notify_input_report(s_mouse_input_report_handle, s_mouse_report,
                        sizeof(s_mouse_report));
    clear_mouse_report();
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

static hid_ble_scan_entry_t s_scan_buf[HID_BLE_SCAN_MAX];
static int s_scan_count;
static SemaphoreHandle_t s_scan_done;
static SemaphoreHandle_t s_scan_mutex;

static void parse_adv_name(const uint8_t *data, int len, char *out, int outlen)
{
    out[0] = 0;
    int i = 0;
    while (i + 1 < len) {
        int alen = data[i];
        if (alen <= 0 || i + alen >= len) break;
        uint8_t ad_type = data[i + 1];
        const uint8_t *val = &data[i + 2];
        int vlen = alen - 1;
        if (ad_type == 0x09 || ad_type == 0x08) {
            int n = vlen < outlen - 1 ? vlen : outlen - 1;
            memcpy(out, val, n);
            out[n] = 0;
            return;
        }
        i += alen + 1;
    }
}

static int scan_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        const ble_addr_t *a = &event->disc.addr;
        for (int i = 0; i < s_scan_count; ++i) {
            if (s_scan_buf[i].addr_type == a->type &&
                memcmp(s_scan_buf[i].addr, a->val, 6) == 0) {
                if (event->disc.rssi > s_scan_buf[i].rssi) s_scan_buf[i].rssi = event->disc.rssi;
                if (!s_scan_buf[i].name[0]) {
                    parse_adv_name(event->disc.data, event->disc.length_data,
                                   s_scan_buf[i].name, sizeof(s_scan_buf[i].name));
                }
                return 0;
            }
        }
        if (s_scan_count < HID_BLE_SCAN_MAX) {
            hid_ble_scan_entry_t *e = &s_scan_buf[s_scan_count++];
            e->addr_type = a->type;
            memcpy(e->addr, a->val, 6);
            e->rssi = event->disc.rssi;
            parse_adv_name(event->disc.data, event->disc.length_data, e->name, sizeof(e->name));
            ESP_LOGI(TAG, "scan: +[%d] %02X:%02X:%02X:%02X:%02X:%02X rssi=%d name=%s",
                     s_scan_count - 1,
                     a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0],
                     event->disc.rssi, e->name);
        } else {
            ESP_LOGD(TAG, "scan: buffer full, dropping rssi=%d", event->disc.rssi);
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        ESP_LOGI(TAG, "scan complete: %d unique advertisers", s_scan_count);
        if (s_scan_done) xSemaphoreGive(s_scan_done);
    }
    return 0;
}

int hid_ble_scan(int duration_ms, hid_ble_scan_entry_t *out, int max)
{
    if (!out || max <= 0) return -1;
    if (duration_ms <= 0) duration_ms = 4000;
    if (duration_ms > 10000) duration_ms = 10000;

    if (!s_scan_mutex) s_scan_mutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -2;
    if (!s_scan_done) s_scan_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_scan_done, 0);

    s_scan_count = 0;
    memset(s_scan_buf, 0, sizeof(s_scan_buf));

    bool adv_was_on = (s_conn_handle == BLE_HS_CONN_HANDLE_NONE);
    if (adv_was_on) ble_gap_adv_stop();
    s_advertising = false;
    s_scanning = true;

    struct ble_gap_disc_params dp = {
        .itvl = 0x0050,
        .window = 0x0040,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_addr_type, duration_ms, &dp, scan_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
        s_scanning = false;
        if (adv_was_on) start_advertising();
        xSemaphoreGive(s_scan_mutex);
        return -3;
    }

    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(duration_ms + 1000));
    ble_gap_disc_cancel();
    s_scanning = false;
    if (adv_was_on) start_advertising();

    int n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan_buf, n * sizeof(hid_ble_scan_entry_t));
    xSemaphoreGive(s_scan_mutex);
    return n;
}

esp_err_t hid_ble_init(const char *device_name)
{
    strlcpy(s_dev_name, device_name ? device_name : "esp32-hid", sizeof(s_dev_name));
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

    /* NimBLE 配对密钥持久化到 NVS（nimble_bond 命名空间）：
     * 不启用则密钥仅存 RAM，重启/OTA 后丢失，已配对主机重连加密失败
     * 陷入"连上→拒绝→断开→重连"死循环 */
    ble_store_config_init();

    /* HID 全部特征与 CCCD 强制加密（HOGP 规范要求，macOS 强校验）：
     * 加密不足的读/写/订阅会返回 Insufficient Authentication，
     * 促使主机发起 SMP 配对；不可用 AUTHEN（Just Works 无 MITM，
     * 若要求会直接配对失败） */
    ble_gatts_set_clt_cfg_perm_flags(BLE_ATT_F_READ | BLE_ATT_F_WRITE |
                                     BLE_ATT_F_WRITE_ENC);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(GATT_SERVICES);
    if (rc) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(GATT_SERVICES);
    if (rc) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(s_dev_name);

    nimble_port_freertos_init(host_task);
    xTaskCreate(adv_watchdog_task, "ble_advwd", 6144, NULL, 3, NULL);
    return ESP_OK;
}
