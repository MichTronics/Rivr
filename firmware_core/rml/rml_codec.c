#include "rml.h"

#if RIVR_ENABLE_RML

#include <string.h>

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

bool rml_encode(const rml_message_t *msg,
                uint8_t *out,
                size_t out_cap,
                size_t *out_len)
{
    if (out_len) {
        *out_len = 0u;
    }
    if (!msg || !out || !out_len || msg->payload_len > RML_MAX_PAYLOAD) {
        g_rml_stats.rml_encode_fail++;
        return false;
    }
    size_t total = RML_WIRE_HDR_LEN + (size_t)msg->payload_len;
    if (out_cap < total) {
        g_rml_stats.rml_encode_fail++;
        return false;
    }

    out[0] = RML_MAGIC;
    out[1] = RML_VERSION;
    out[2] = msg->type;
    out[3] = msg->supersedes ? (uint8_t)(msg->flags | RML_FLAG_SUPERSEDES) : msg->flags;
    put_u32(out + 4, msg->msg_id);
    put_u32(out + 8, msg->thread_id);
    put_u32(out + 12, msg->prev_id);
    put_u16(out + 16, msg->sender_id);
    put_u16(out + 18, msg->target_id);
    out[20] = msg->intent;
    out[21] = msg->priority;
    out[22] = msg->ttl;
    out[23] = msg->reliability;
    put_u16(out + 24, msg->expires_s);
    put_u16(out + 26, msg->payload_len);
    if (msg->payload_len > 0u) {
        memcpy(out + RML_WIRE_HDR_LEN, msg->payload, msg->payload_len);
    }
    *out_len = total;
    return true;
}

bool rml_decode(const uint8_t *buf,
                size_t len,
                rml_message_t *out)
{
    if (!buf || !out || len < RML_WIRE_HDR_LEN) {
        g_rml_stats.rml_decode_fail++;
        return false;
    }
    if (buf[0] != RML_MAGIC) {
        g_rml_stats.rml_rx_bad_magic++;
        g_rml_stats.rml_decode_fail++;
        return false;
    }
    if (buf[1] != RML_VERSION) {
        g_rml_stats.rml_rx_bad_version++;
        g_rml_stats.rml_decode_fail++;
        return false;
    }

    uint16_t payload_len = get_u16(buf + 26);
    if (payload_len > RML_MAX_PAYLOAD) {
        g_rml_stats.rml_rx_too_large++;
        g_rml_stats.rml_decode_fail++;
        return false;
    }
    if (len < RML_WIRE_HDR_LEN + (size_t)payload_len) {
        g_rml_stats.rml_decode_fail++;
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->magic = buf[0];
    out->version = buf[1];
    out->type = buf[2];
    out->flags = buf[3];
    out->supersedes = (buf[3] & RML_FLAG_SUPERSEDES) != 0u;
    out->msg_id = get_u32(buf + 4);
    out->thread_id = get_u32(buf + 8);
    out->prev_id = get_u32(buf + 12);
    out->sender_id = get_u16(buf + 16);
    out->target_id = get_u16(buf + 18);
    out->state_type = (uint16_t)out->thread_id;
    out->intent = buf[20];
    out->priority = buf[21];
    out->ttl = buf[22];
    out->reliability = buf[23];
    out->expires_s = get_u16(buf + 24);
    out->payload_len = payload_len;
    if (payload_len > 0u) {
        memcpy(out->payload, buf + RML_WIRE_HDR_LEN, payload_len);
    }
    return true;
}

#endif /* RIVR_ENABLE_RML */
