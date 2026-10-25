/*
 * V2.1 envelope decoder + dispatcher.
 *
 *   {
 *     "header":  { msg_id, type, device_id, timestamp, source, [ref_msg_id], [seq] },
 *     "routing": { ... },
 *     "payload": { ... }
 *   }
 *
 * type=="action"  -> parsed into action_msg_t and enqueued for the executor task,
 *                    which will emit its own ACK after the HID side runs.
 *
 * everything else -> dispatched synchronously through cmd_router; ACK with the
 *                    handler's result fields merged into payload.
 *
 * Maintains a 100-entry msg_id dedup ring and an optional strict_seq monotonic
 * check (driven by app_config_get()->strict_seq).
 */
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "app_config.h"
#include "cmd_router.h"
#include "executor.h"
#include "ws_client.h"

static const char *TAG = "proto";

#define DEDUP_RING 100
static char     s_dedup[DEDUP_RING][PROTO_MSGID_LEN];
static int      s_dedup_pos = 0;
static uint32_t s_last_seq  = 0;

static bool dedup_seen(const char *id)
{
    if (!id || !id[0]) return false;
    for (int i = 0; i < DEDUP_RING; ++i) {
        if (s_dedup[i][0] && strcmp(s_dedup[i], id) == 0) return true;
    }
    return false;
}

static bool json_bool_or(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static bool action_needs_ack(const cJSON *payload)
{
    if (!cJSON_IsObject(payload)) return false;
    bool need_ack = json_bool_or(payload, "need_ack", false);
    need_ack = json_bool_or(payload, "loopback", need_ack);
    return need_ack;
}

static void dedup_remember(const char *id)
{
    if (!id || !id[0]) return;
    strlcpy(s_dedup[s_dedup_pos], id, PROTO_MSGID_LEN);
    s_dedup_pos = (s_dedup_pos + 1) % DEDUP_RING;
}

void protocol_make_uuid(char dst[37])
{
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = (uint8_t)(esp_random() & 0xff);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(dst, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
        b[10],b[11],b[12],b[13],b[14],b[15]);
}

static cJSON *make_envelope(const char *type, const char *ref_msg_id)
{
    cJSON *env = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddObjectToObject(env, "header");
    char uuid[37];
    protocol_make_uuid(uuid);
    cJSON_AddStringToObject(hdr, "msg_id", uuid);
    cJSON_AddStringToObject(hdr, "type", type);
    cJSON_AddStringToObject(hdr, "device_id", app_config_get()->device_id);
    cJSON_AddNumberToObject(hdr, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(hdr, "source", "esp32");
    if (ref_msg_id && ref_msg_id[0]) {
        cJSON_AddStringToObject(hdr, "ref_msg_id", ref_msg_id);
    }
    cJSON_AddObjectToObject(env, "routing");
    cJSON_AddObjectToObject(env, "payload");
    return env;
}

static void add_error_fields(cJSON *payload, const char *code, const char *message)
{
    if (!payload || (!code && !message)) return;
    cJSON *err = cJSON_AddObjectToObject(payload, "error");
    if (code) {
        cJSON_AddStringToObject(err, "code", code);
        cJSON_AddStringToObject(payload, "err_code", code);
    }
    if (message) {
        cJSON_AddStringToObject(err, "message", message);
        cJSON_AddStringToObject(payload, "err_msg", message);
    }
}

cJSON *protocol_make_ack(const char *ref_msg_id, bool ok, int exec_time_ms,
                         const char *err_code, const char *err_msg)
{
    cJSON *env = make_envelope("ack", ref_msg_id);
    cJSON *pl  = cJSON_GetObjectItem(env, "payload");
    cJSON_AddBoolToObject(pl, "ok", ok);
    cJSON_AddStringToObject(pl, "status", ok ? "ok" : "failed");
    cJSON_AddNumberToObject(pl, "exec_time_ms", exec_time_ms);
    if (!ok) add_error_fields(pl, err_code, err_msg);
    return env;
}

cJSON *protocol_make_error(const char *ref_msg_id, const char *code, const char *msg)
{
    cJSON *env = make_envelope("error", ref_msg_id);
    cJSON *pl  = cJSON_GetObjectItem(env, "payload");
    if (code) cJSON_AddStringToObject(pl, "code", code);
    if (msg) {
        cJSON_AddStringToObject(pl, "msg", msg);
        cJSON_AddStringToObject(pl, "message", msg);
    }
    return env;
}

static cJSON *make_ack_with_result(const char *ref_msg_id, int code, int exec_time_ms,
                                   const char *err_msg, cJSON *result_owned)
{
    cJSON *env = make_envelope("ack", ref_msg_id);
    cJSON *pl  = cJSON_GetObjectItem(env, "payload");
    bool ok = code == 0;
    cJSON_AddBoolToObject(pl, "ok", ok);
    cJSON_AddStringToObject(pl, "status", ok ? "ok" : "failed");
    cJSON_AddNumberToObject(pl, "exec_time_ms", exec_time_ms);
    if (!ok) {
        char codebuf[12];
        snprintf(codebuf, sizeof codebuf, "E%03d", code);
        add_error_fields(pl, codebuf, err_msg && err_msg[0] ? err_msg : NULL);
    }
    if (result_owned) {
        cJSON_AddItemToObject(pl, "result", result_owned);
    }
    return env;
}

static bool parse_action(const cJSON *root, const cJSON *payload,
                         action_msg_t *am, char *err, int errlen)
{
    (void)root;
    if (!cJSON_IsObject(payload)) {
        snprintf(err, errlen, "action payload missing");
        return false;
    }
    const cJSON *jt = cJSON_GetObjectItem(payload, "type");
    if (!cJSON_IsString(jt)) {
        snprintf(err, errlen, "action.type missing");
        return false;
    }
    strlcpy(am->action.type, jt->valuestring, sizeof am->action.type);
    const cJSON *jk = cJSON_GetObjectItem(payload, "key");
    if (cJSON_IsString(jk)) strlcpy(am->action.key, jk->valuestring, sizeof am->action.key);
    const cJSON *jc = cJSON_GetObjectItem(payload, "content");
    if (!cJSON_IsString(jc)) jc = cJSON_GetObjectItem(payload, "text");
    if (cJSON_IsString(jc)) strlcpy(am->action.text, jc->valuestring, sizeof am->action.text);
    const cJSON *ji = cJSON_GetObjectItem(payload, "interval_ms");
    if (cJSON_IsNumber(ji)) am->action.interval_ms = ji->valueint;
    const cJSON *jd = cJSON_GetObjectItem(payload, "delay_ms");
    if (cJSON_IsNumber(jd)) am->action.delay_ms = jd->valueint;
    const cJSON *jx = cJSON_GetObjectItem(payload, "x");
    const cJSON *jy = cJSON_GetObjectItem(payload, "y");
    if (cJSON_IsNumber(jx)) am->action.x = jx->valueint;
    if (cJSON_IsNumber(jy)) am->action.y = jy->valueint;
    const cJSON *jb = cJSON_GetObjectItem(payload, "button");
    if (cJSON_IsNumber(jb)) am->action.button = (uint8_t)jb->valueint;
    am->action.macro_steps = cJSON_GetObjectItem(payload, "steps");
    return true;
}

void protocol_feed(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "bad json frame (%d bytes)", len);
        return;
    }
    cJSON *hdr     = cJSON_GetObjectItem(root, "header");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsObject(hdr)) {
        ESP_LOGW(TAG, "no header");
        cJSON_Delete(root);
        return;
    }
    const cJSON *jtype  = cJSON_GetObjectItem(hdr, "type");
    const cJSON *jmsgid = cJSON_GetObjectItem(hdr, "msg_id");
    const cJSON *jseq   = cJSON_GetObjectItem(hdr, "seq");
    const char *type    = cJSON_IsString(jtype)  ? jtype->valuestring  : NULL;
    const char *msg_id  = cJSON_IsString(jmsgid) ? jmsgid->valuestring : NULL;
    bool need_ack = action_needs_ack(payload);
    if (!type) {
        ESP_LOGW(TAG, "no type");
        cJSON_Delete(root);
        return;
    }

    if (!strcmp(type, "ack") || !strcmp(type, "error") ||
        !strcmp(type, "register_ack") || !strcmp(type, "hello_ack")) {
        cJSON_Delete(root);
        return;
    }

    /* 注册鉴权挑战：转交 ws_client 计算 HMAC 并重新 register */
    if (!strcmp(type, "auth_challenge")) {
        const cJSON *jn = cJSON_GetObjectItem(payload, "nonce");
        if (cJSON_IsString(jn) && jn->valuestring[0]) {
            ws_client_handle_auth_challenge(jn->valuestring);
        }
        cJSON_Delete(root);
        return;
    }

    if (msg_id && dedup_seen(msg_id)) {
        ESP_LOGW(TAG, "dup msg_id %s ignored", msg_id);
        if (need_ack) ws_client_send(protocol_make_ack(msg_id, true, 0, NULL, NULL));
        cJSON_Delete(root);
        return;
    }
    if (msg_id) dedup_remember(msg_id);

    if (app_config_get()->strict_seq && cJSON_IsNumber(jseq)) {
        uint32_t seq = (uint32_t)jseq->valuedouble;
        if (seq != 0 && seq <= s_last_seq) {
            ESP_LOGW(TAG, "out-of-order seq %lu (last %lu)",
                     (unsigned long)seq, (unsigned long)s_last_seq);
            if (strcmp(type, "action") != 0 || need_ack) {
                ws_client_send(protocol_make_ack(msg_id, false, 0, "E002", "out of order"));
            }
            cJSON_Delete(root);
            return;
        }
        if (seq > s_last_seq) s_last_seq = seq;
    }

    if (!strcmp(type, "action")) {
        action_msg_t *am = calloc(1, sizeof *am);
        if (!am) {
            cJSON_Delete(root);
            return;
        }
        if (msg_id) strlcpy(am->msg_id, msg_id, sizeof am->msg_id);
        if (cJSON_IsNumber(jseq)) am->seq = (uint32_t)jseq->valuedouble;
        am->need_ack = need_ack;
        char err[80] = {0};
        if (!parse_action(root, payload, am, err, sizeof err)) {
            if (need_ack) ws_client_send(protocol_make_ack(msg_id, false, 0, "E004", err));
            free(am);
            cJSON_Delete(root);
            return;
        }
        am->action.root = root;
        if (executor_enqueue(am) != ESP_OK) {
            if (need_ack) ws_client_send(protocol_make_ack(msg_id, false, 0, "E005", "queue full"));
            free(am);
            cJSON_Delete(root);
        }
        return;
    }

    if (!strcmp(type, "ping")) {
        cJSON *env = make_envelope("pong", msg_id);
        ws_client_send(env);
        cJSON_Delete(root);
        return;
    }

    int64_t t0 = esp_timer_get_time();
    char err[96] = {0};
    cJSON *result = cJSON_CreateObject();
    int code = cmd_router_dispatch(type, payload, result, err, sizeof err);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);

    if (code == 0 && result->child != NULL) {
        ws_client_send(make_ack_with_result(msg_id, 0, ms, NULL, result));
    } else {
        cJSON_Delete(result);
        ws_client_send(make_ack_with_result(
            msg_id,
            code,
            ms,
            code == 0 ? NULL : err,
            NULL
        ));
    }
    cJSON_Delete(root);
}
