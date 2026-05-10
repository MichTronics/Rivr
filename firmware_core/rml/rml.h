#ifndef RIVR_RML_H
#define RIVR_RML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RML_MAGIC       0xA7u
#define RML_VERSION     1u
#define RML_WIRE_HDR_LEN 28u
#define RML_FLAG_SUPERSEDES 0x01u

#ifndef RIVR_ENABLE_RML
#  define RIVR_ENABLE_RML 0
#endif

#ifndef RML_MAX_PAYLOAD
#  define RML_MAX_PAYLOAD 160u
#endif

#ifndef RML_SEEN_CACHE_SIZE
#  define RML_SEEN_CACHE_SIZE 64u
#endif

#ifndef RML_STATUS_CACHE_SIZE
#  define RML_STATUS_CACHE_SIZE 16u
#endif

#ifndef RML_THREAD_CACHE_SIZE
#  define RML_THREAD_CACHE_SIZE 16u
#endif

#ifndef RML_THREAD_TEXT_MAX
#  define RML_THREAD_TEXT_MAX 160u
#endif

#ifndef RML_REPAIR_CACHE_SIZE
#  define RML_REPAIR_CACHE_SIZE 8u
#endif

typedef enum {
    RML_TYPE_CHAT = 1,
    RML_TYPE_STATUS,
    RML_TYPE_DELTA,
    RML_TYPE_ACK_HINT,
    RML_TYPE_THREAD_SYNC,
    RML_TYPE_TOPIC_ADV,
    RML_TYPE_CONTROL,
    RML_TYPE_REPAIR_REQUEST,
} rml_type_t;

typedef enum {
    RML_DELTA_APPEND = 1,
    RML_DELTA_REPLACE,
    RML_DELTA_DELETE_RANGE,
} rml_delta_op_t;

typedef enum {
    RML_INTENT_CHAT = 1,
    RML_INTENT_PRIVATE,
    RML_INTENT_GROUP,
    RML_INTENT_EMERGENCY,
    RML_INTENT_TELEMETRY,
    RML_INTENT_STATUS,
    RML_INTENT_SYSTEM,
} rml_intent_t;

typedef enum {
    RML_REL_OPPORTUNISTIC = 0,
    RML_REL_IMPLICIT_ACK,
    RML_REL_SPARSE_ACK,
    RML_REL_REPAIR_REQUEST,
} rml_reliability_t;

typedef enum {
    RML_ROLE_UNKNOWN = 0,
    RML_ROLE_CLIENT,
    RML_ROLE_REPEATER,
    RML_ROLE_GATEWAY,
} rml_role_t;

typedef enum {
    RML_REASON_OK = 0,
    RML_REASON_SEEN,
    RML_REASON_EXPIRED,
    RML_REASON_TTL_ZERO,
    RML_REASON_DUTY_BLOCKED,
    RML_REASON_QUEUE_FULL,
    RML_REASON_REPLACED_BY_NEWER_STATUS,
    RML_REASON_MISSING_PREV,
    RML_REASON_DELTA_APPLIED,
    RML_REASON_DELTA_FAILED,
    RML_REASON_REPAIR_NEEDED,
} rml_reason_t;

typedef struct {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    uint32_t msg_id;
    uint32_t thread_id;
    uint32_t prev_id;
    uint16_t sender_id;
    uint16_t target_id;
    uint8_t  intent;
    uint8_t  priority;
    uint8_t  ttl;
    uint8_t  reliability;
    uint16_t expires_s;
    bool supersedes;
    uint16_t state_type;
    uint16_t soft_ttl_s;
    uint32_t created_ms;
    uint16_t payload_len;
    uint8_t  payload[RML_MAX_PAYLOAD];
} rml_message_t;

typedef struct {
    uint16_t local_id;
    uint32_t now_s;
    uint32_t now_ms;
    bool has_time;
    bool hard_radio_ok;
    bool duty_cycle_blocked;
    uint32_t duty_budget_us;
    uint32_t duty_remaining_us;
    int16_t rssi_dbm;
    int8_t snr_db;
    uint8_t local_role;
    uint8_t queue_depth;
    uint8_t queue_capacity;
    uint8_t queue_pressure;
    bool route_hint_valid;
    bool target_hint_useful;
} rml_context_t;

typedef struct {
    uint8_t op;
    uint16_t offset;
    uint16_t delete_len;
    uint16_t data_len;
    uint8_t data[RML_MAX_PAYLOAD];
} rml_delta_t;

typedef struct {
    bool used;
    uint16_t sender_id;
    uint32_t thread_id;
    uint32_t msg_id;
    uint32_t prev_id;
    uint32_t timestamp_ms;
    uint32_t soft_expire_ms;
    uint16_t payload_len;
    uint8_t payload[RML_THREAD_TEXT_MAX];
} rml_thread_entry_t;

typedef struct {
    rml_thread_entry_t entries[RML_THREAD_CACHE_SIZE];
    uint16_t next_slot;
} rml_thread_cache_t;

typedef struct {
    uint32_t missing_msg_id;
    uint32_t thread_id;
    uint16_t sender_id;
} rml_repair_request_t;

typedef struct {
    bool accept;
    bool relay;
    uint16_t delay_ms;
    uint8_t next_ttl;
    uint8_t reason;
    uint16_t repair_delay_ms;
    bool forward_reconstructed;
    bool repair_needed;
    rml_repair_request_t repair;
} rml_policy_decision_t;

typedef struct {
    uint32_t rml_rx_total;
    uint32_t rml_rx_bad_magic;
    uint32_t rml_rx_bad_version;
    uint32_t rml_rx_too_large;
    uint32_t rml_rx_seen_drop;
    uint32_t rml_rx_expired_drop;
    uint32_t rml_relay_total;
    uint32_t rml_relay_ttl_drop;
    uint32_t rml_encode_fail;
    uint32_t rml_decode_fail;
} rml_stats_t;

extern rml_stats_t g_rml_stats;

bool rml_init(void);

bool rml_encode(const rml_message_t *msg,
                uint8_t *out,
                size_t out_cap,
                size_t *out_len);

bool rml_decode(const uint8_t *buf,
                size_t len,
                rml_message_t *out);

rml_policy_decision_t rml_policy_decide(const rml_message_t *msg,
                                        const rml_context_t *ctx);

void rml_mark_seen(uint32_t msg_id);
bool rml_seen(uint32_t msg_id);

bool rml_make_chat(uint16_t sender_id,
                   uint16_t target_id,
                   uint32_t thread_id,
                   const uint8_t *payload,
                   uint16_t payload_len,
                   rml_message_t *out);

bool rml_delta_payload_encode(const rml_delta_t *delta,
                              uint8_t *out,
                              uint16_t out_cap,
                              uint16_t *out_len);

bool rml_delta_payload_decode(const uint8_t *buf,
                              uint16_t len,
                              rml_delta_t *out);

bool rml_make_delta(uint16_t sender_id,
                    uint16_t target_id,
                    uint32_t thread_id,
                    uint32_t msg_id,
                    uint32_t prev_id,
                    const rml_delta_t *delta,
                    rml_message_t *out);

bool rml_make_repair_request(uint16_t sender_id,
                             uint16_t target_id,
                             const rml_repair_request_t *repair,
                             rml_message_t *out);

bool rml_repair_request_decode(const rml_message_t *msg,
                               rml_repair_request_t *out);

bool rml_thread_cache_get(uint16_t sender_id,
                          uint32_t thread_id,
                          rml_thread_entry_t *out);

void rml_seen_reset(void);
void rml_policy_reset(void);
void rml_thread_cache_reset(void);
void rml_stats_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_RML_H */
