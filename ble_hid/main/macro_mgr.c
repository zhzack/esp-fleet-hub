#include "macro_mgr.h"
#include "executor.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "macro_mgr";
#define MACRO_NVS_NS  "macros"
#define MACRO_NVS_KEY "macro_list"
#define MAX_BLOB_SIZE 16384

static const char DEFAULT_MACROS_JSON[] = "["
"{\"id\":\"lock\",\"name\":\"锁定电脑\",\"icon\":\"lock\",\"desc\":\"Win + L\","
"\"payload\":{\"type\":\"keyboard_tap\",\"key\":\"KEY_GUI_L\"}},"
"{\"id\":\"desktop\",\"name\":\"显示桌面\",\"icon\":\"desktop\",\"desc\":\"Win + D\","
"\"payload\":{\"type\":\"keyboard_tap\",\"key\":\"KEY_GUI_D\"}},"
"{\"id\":\"taskmgr\",\"name\":\"任务管理器\",\"icon\":\"activity\",\"desc\":\"Ctrl + Shift + Esc\","
"\"payload\":{\"type\":\"macro\",\"steps\":["
"{\"type\":\"keyboard_down\",\"key\":\"KEY_LEFTCTRL\"},"
"{\"type\":\"keyboard_down\",\"key\":\"KEY_LEFTSHIFT\"},"
"{\"type\":\"keyboard_tap\",\"key\":\"KEY_ESC\"},"
"{\"type\":\"keyboard_up\",\"key\":\"KEY_LEFTSHIFT\"},"
"{\"type\":\"keyboard_up\",\"key\":\"KEY_LEFTCTRL\"}]}},"
"{\"id\":\"mute\",\"name\":\"静音开关\",\"icon\":\"volume-x\",\"desc\":\"消费类音频静音\","
"\"payload\":{\"type\":\"consumer\",\"key\":\"KEY_MUTE\"}},"
"{\"id\":\"volup\",\"name\":\"增大音量\",\"icon\":\"volume-2\",\"desc\":\"音量增加\","
"\"payload\":{\"type\":\"consumer\",\"key\":\"KEY_VOL_UP\"}},"
"{\"id\":\"voldown\",\"name\":\"减小音量\",\"icon\":\"volume-1\",\"desc\":\"音量减小\","
"\"payload\":{\"type\":\"consumer\",\"key\":\"KEY_VOL_DOWN\"}},"
"{\"id\":\"playpause\",\"name\":\"播放 / 暂停\",\"icon\":\"play\",\"desc\":\"多媒体播放暂停\","
"\"payload\":{\"type\":\"consumer\",\"key\":\"KEY_PLAY_PAUSE\"}},"
"{\"id\":\"shutdown\",\"name\":\"安全关机\",\"icon\":\"power\",\"desc\":\"Win + X -> U -> U\","
"\"payload\":{\"type\":\"macro\",\"steps\":["
"{\"type\":\"keyboard_down\",\"key\":\"KEY_LEFTGUI\"},"
"{\"type\":\"keyboard_tap\",\"key\":\"KEY_X\"},"
"{\"type\":\"keyboard_up\",\"key\":\"KEY_LEFTGUI\"},"
"{\"type\":\"delay\",\"delay_ms\":300},"
"{\"type\":\"keyboard_tap\",\"key\":\"KEY_U\"},"
"{\"type\":\"delay\",\"delay_ms\":200},"
"{\"type\":\"keyboard_tap\",\"key\":\"KEY_U\"}]}}"
"]";

esp_err_t macro_mgr_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MACRO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    size_t req_len = 0;
    err = nvs_get_blob(h, MACRO_NVS_KEY, NULL, &req_len);
    if (err == ESP_ERR_NVS_NOT_FOUND || req_len == 0) {
        ESP_LOGI(TAG, "no macros found in NVS, writing factory defaults");
        err = nvs_set_blob(h, MACRO_NVS_KEY, DEFAULT_MACROS_JSON, strlen(DEFAULT_MACROS_JSON) + 1);
        if (err == ESP_OK) {
            nvs_commit(h);
        }
    }
    nvs_close(h);
    return ESP_OK;
}

cJSON *macro_mgr_get_all(void)
{
    nvs_handle_t h;
    if (nvs_open(MACRO_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return cJSON_Parse(DEFAULT_MACROS_JSON);
    }

    size_t req_len = 0;
    if (nvs_get_blob(h, MACRO_NVS_KEY, NULL, &req_len) != ESP_OK || req_len == 0) {
        nvs_close(h);
        return cJSON_Parse(DEFAULT_MACROS_JSON);
    }

    if (req_len > MAX_BLOB_SIZE) req_len = MAX_BLOB_SIZE;
    char *buf = malloc(req_len + 1);
    if (!buf) {
        nvs_close(h);
        return cJSON_Parse(DEFAULT_MACROS_JSON);
    }

    if (nvs_get_blob(h, MACRO_NVS_KEY, buf, &req_len) != ESP_OK) {
        free(buf);
        nvs_close(h);
        return cJSON_Parse(DEFAULT_MACROS_JSON);
    }
    buf[req_len] = '\0';
    nvs_close(h);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return cJSON_Parse(DEFAULT_MACROS_JSON);
    }
    return root;
}

static esp_err_t save_macro_array(cJSON *arr)
{
    if (!arr) return ESP_ERR_INVALID_ARG;
    char *str = cJSON_PrintUnformatted(arr);
    if (!str) return ESP_ERR_NO_MEM;

    size_t len = strlen(str) + 1;
    if (len > MAX_BLOB_SIZE) {
        free(str);
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(MACRO_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, MACRO_NVS_KEY, str, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(str);
    return err;
}

esp_err_t macro_mgr_save(const cJSON *macro_obj)
{
    if (!macro_obj || !cJSON_IsObject(macro_obj)) return ESP_ERR_INVALID_ARG;
    const cJSON *jid = cJSON_GetObjectItem(macro_obj, "id");
    if (!jid || !cJSON_IsString(jid) || !jid->valuestring[0]) return ESP_ERR_INVALID_ARG;

    cJSON *arr = macro_mgr_get_all();
    if (!arr) arr = cJSON_CreateArray();

    int count = cJSON_GetArraySize(arr);
    bool replaced = false;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *item_id = cJSON_GetObjectItem(item, "id");
        if (item_id && cJSON_IsString(item_id) && strcmp(item_id->valuestring, jid->valuestring) == 0) {
            cJSON_ReplaceItemInArray(arr, i, cJSON_Duplicate((cJSON*)macro_obj, true));
            replaced = true;
            break;
        }
    }

    if (!replaced) {
        cJSON_AddItemToArray(arr, cJSON_Duplicate((cJSON*)macro_obj, true));
    }

    esp_err_t err = save_macro_array(arr);
    cJSON_Delete(arr);
    return err;
}

esp_err_t macro_mgr_delete(const char *id)
{
    if (!id || !id[0]) return ESP_ERR_INVALID_ARG;
    cJSON *arr = macro_mgr_get_all();
    if (!arr) return ESP_ERR_NOT_FOUND;

    int count = cJSON_GetArraySize(arr);
    int found_idx = -1;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *item_id = cJSON_GetObjectItem(item, "id");
        if (item_id && cJSON_IsString(item_id) && strcmp(item_id->valuestring, id) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx >= 0) {
        cJSON_DeleteItemFromArray(arr, found_idx);
        save_macro_array(arr);
        cJSON_Delete(arr);
        return ESP_OK;
    }

    cJSON_Delete(arr);
    return ESP_ERR_NOT_FOUND;
}

int macro_mgr_exec(const char *id, char *err, int errlen)
{
    if (!id || !id[0]) {
        if (err && errlen) snprintf(err, errlen, "id missing");
        return 400;
    }

    cJSON *arr = macro_mgr_get_all();
    if (!arr) {
        if (err && errlen) snprintf(err, errlen, "no macros");
        return 404;
    }

    cJSON *target = NULL;
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *item_id = cJSON_GetObjectItem(item, "id");
        if (item_id && cJSON_IsString(item_id) && strcmp(item_id->valuestring, id) == 0) {
            target = item;
            break;
        }
    }

    if (!target) {
        if (err && errlen) snprintf(err, errlen, "macro '%s' not found", id);
        cJSON_Delete(arr);
        return 404;
    }

    cJSON *payload = cJSON_GetObjectItem(target, "payload");
    if (!payload) payload = target;

    char errcode[8] = {0};
    int ret = executor_run_payload_sync(payload, errcode, sizeof(errcode), err, errlen);
    cJSON_Delete(arr);
    return ret;
}
