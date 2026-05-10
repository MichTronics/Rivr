#include "rml.h"

#if RIVR_ENABLE_RML

#include "firmware_core/rivr_log.h"

#include <string.h>

#define TAG "RML"

static uint32_t rml_hash_init(void)
{
    return 2166136261u;
}

static uint32_t rml_hash_byte(uint32_t h, uint8_t b)
{
    h ^= b;
    return h * 16777619u;
}

static uint32_t rml_hash_u16(uint32_t h, uint16_t v)
{
    h = rml_hash_byte(h, (uint8_t)(v & 0xFFu));
    return rml_hash_byte(h, (uint8_t)(v >> 8));
}

static uint32_t rml_hash_u32(uint32_t h, uint32_t v)
{
    h = rml_hash_byte(h, (uint8_t)(v & 0xFFu));
    h = rml_hash_byte(h, (uint8_t)((v >> 8) & 0xFFu));
    h = rml_hash_byte(h, (uint8_t)((v >> 16) & 0xFFu));
    return rml_hash_byte(h, (uint8_t)((v >> 24) & 0xFFu));
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

bool rml_init(void)
{
    rml_stats_reset();
    rml_seen_reset();
    rml_policy_reset();
    RIVR_LOGI(TAG, "init ok");
    return true;
}

bool rml_make_chat(uint16_t sender_id,
                   uint16_t target_id,
                   uint32_t thread_id,
                   const uint8_t *payload,
                   uint16_t payload_len,
                   rml_message_t *out)
{
    if (!out || payload_len > RML_MAX_PAYLOAD || (payload_len > 0u && !payload)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->magic = RML_MAGIC;
    out->version = RML_VERSION;
    out->type = RML_TYPE_CHAT;
    out->flags = 0u;
    out->thread_id = thread_id;
    out->state_type = (uint16_t)thread_id;
    out->soft_ttl_s = 0u;
    out->created_ms = 0u;
    out->prev_id = 0u;
    out->sender_id = sender_id;
    out->target_id = target_id;
    out->intent = target_id == 0u ? RML_INTENT_CHAT : RML_INTENT_PRIVATE;
    out->priority = 3u;
    out->ttl = 8u;
    out->reliability = RML_REL_OPPORTUNISTIC;
    out->expires_s = 0u;
    out->payload_len = payload_len;
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }

    uint32_t h = rml_hash_init();
    h = rml_hash_u16(h, sender_id);
    h = rml_hash_u16(h, target_id);
    h = rml_hash_u32(h, thread_id);
    h = rml_hash_u16(h, payload_len);
    for (uint16_t i = 0u; i < payload_len; i++) {
        h = rml_hash_byte(h, payload[i]);
    }
    out->msg_id = (h == 0u) ? 1u : h;
    return true;
}

bool rml_delta_payload_encode(const rml_delta_t *delta,
                              uint8_t *out,
                              uint16_t out_cap,
                              uint16_t *out_len)
{
    if (out_len) {
        *out_len = 0u;
    }
    if (!delta || !out || !out_len || delta->data_len > RML_MAX_PAYLOAD) {
        return false;
    }
    uint16_t need = (uint16_t)(7u + delta->data_len);
    if (out_cap < need) {
        return false;
    }
    out[0] = delta->op;
    put_u16(out + 1, delta->offset);
    put_u16(out + 3, delta->delete_len);
    put_u16(out + 5, delta->data_len);
    if (delta->data_len > 0u) {
        memcpy(out + 7, delta->data, delta->data_len);
    }
    *out_len = need;
    return true;
}

bool rml_delta_payload_decode(const uint8_t *buf,
                              uint16_t len,
                              rml_delta_t *out)
{
    if (!buf || !out || len < 7u) {
        return false;
    }
    uint16_t data_len = get_u16(buf + 5);
    if (data_len > RML_MAX_PAYLOAD || len < (uint16_t)(7u + data_len)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->op = buf[0];
    out->offset = get_u16(buf + 1);
    out->delete_len = get_u16(buf + 3);
    out->data_len = data_len;
    if (data_len > 0u) {
        memcpy(out->data, buf + 7, data_len);
    }
    return true;
}

bool rml_make_delta(uint16_t sender_id,
                    uint16_t target_id,
                    uint32_t thread_id,
                    uint32_t msg_id,
                    uint32_t prev_id,
                    const rml_delta_t *delta,
                    rml_message_t *out)
{
    if (!out || !delta) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->magic = RML_MAGIC;
    out->version = RML_VERSION;
    out->type = RML_TYPE_DELTA;
    out->msg_id = msg_id;
    out->thread_id = thread_id;
    out->state_type = (uint16_t)thread_id;
    out->prev_id = prev_id;
    out->sender_id = sender_id;
    out->target_id = target_id;
    out->intent = RML_INTENT_CHAT;
    out->priority = 3u;
    out->ttl = 8u;
    out->reliability = RML_REL_REPAIR_REQUEST;
    return rml_delta_payload_encode(delta, out->payload, RML_MAX_PAYLOAD, &out->payload_len);
}

bool rml_make_repair_request(uint16_t sender_id,
                             uint16_t target_id,
                             const rml_repair_request_t *repair,
                             rml_message_t *out)
{
    if (!repair || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->magic = RML_MAGIC;
    out->version = RML_VERSION;
    out->type = RML_TYPE_REPAIR_REQUEST;
    out->msg_id = repair->missing_msg_id ^ repair->thread_id ^ ((uint32_t)sender_id << 16);
    if (out->msg_id == 0u) {
        out->msg_id = 1u;
    }
    out->thread_id = repair->thread_id;
    out->state_type = (uint16_t)repair->thread_id;
    out->prev_id = repair->missing_msg_id;
    out->sender_id = sender_id;
    out->target_id = target_id;
    out->intent = RML_INTENT_SYSTEM;
    out->priority = 6u;
    out->ttl = 3u;
    out->reliability = RML_REL_OPPORTUNISTIC;
    out->payload_len = 10u;
    put_u32(out->payload + 0, repair->missing_msg_id);
    put_u32(out->payload + 4, repair->thread_id);
    put_u16(out->payload + 8, repair->sender_id);
    return true;
}

bool rml_repair_request_decode(const rml_message_t *msg,
                               rml_repair_request_t *out)
{
    if (!msg || !out || msg->type != RML_TYPE_REPAIR_REQUEST || msg->payload_len < 10u) {
        return false;
    }
    out->missing_msg_id = get_u32(msg->payload + 0);
    out->thread_id = get_u32(msg->payload + 4);
    out->sender_id = get_u16(msg->payload + 8);
    return true;
}

#endif /* RIVR_ENABLE_RML */
