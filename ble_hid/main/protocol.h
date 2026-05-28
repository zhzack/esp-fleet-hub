#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_KEY_LEN     16
#define PROTO_TYPE_LEN    32
#define PROTO_TEXT_LEN    256
#define PROTO_MSGID_LEN   37

typedef struct {
    char     type[PROTO_TYPE_LEN];
    char     key[PROTO_KEY_LEN];
    int32_t  x, y;
    int32_t  delay_ms;
    char     text[PROTO_TEXT_LEN];
    int      interval_ms;
    uint8_t  button;
    cJSON   *macro_steps;        /* borrowed pointer, NULL unless type==macro */
    cJSON   *root;               /* owning root, freed by executor after action runs */
} action_t;

typedef struct {
    char     msg_id[PROTO_MSGID_LEN];
    uint32_t seq;
    action_t action;
} action_msg_t;

/** Feed raw text frame from WS layer. Frees nothing; caller owns buffer. */
void protocol_feed(const char *data, int len);

/** Build an ACK envelope. Returns owning cJSON*. */
cJSON *protocol_make_ack(const char *ref_msg_id, bool ok, int exec_time_ms,
                         const char *err_code, const char *err_msg);

/** Build an error envelope. */
cJSON *protocol_make_error(const char *ref_msg_id, const char *code, const char *msg);

/** Random RFC-4122 v4 UUID into dst[37]. */
void protocol_make_uuid(char dst[37]);

#ifdef __cplusplus
}
#endif
