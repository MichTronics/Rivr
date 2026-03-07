/**
 * @file  retry_table.c
 * @brief ACK-wait / retry table implementation.
 *
 * See retry_table.h for the full design description.
 */

#include "retry_table.h"
#include "rivr_metrics.h"
#include "rivr_log.h"
#include <string.h>

#define TAG "RETRY"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/**
 * Patch the pkt_id bytes in a wire frame and recompute the CRC.
 *
 * Wire layout offsets (see protocol.h):
 *   [19..20]              pkt_id  u16 LE            (PKT_ID_BYTE_OFFSET)
 *   [21]                  payload_len u8
 *   [22]                  loop_guard u8
 *   [23 .. 23+PL-1]       payload
 *   [23+PL .. 23+PL+1]    CRC-16/CCITT LE
 */
static void patch_pkt_id_and_crc(uint8_t *data, uint8_t len, uint16_t new_pkt_id)
{
    /* Patch pkt_id (LE, bytes 19–20) */
    data[PKT_ID_BYTE_OFFSET]     = (uint8_t)(new_pkt_id & 0xFFu);
    data[PKT_ID_BYTE_OFFSET + 1] = (uint8_t)(new_pkt_id >>   8u);

    /* Recompute CRC over [0 .. RIVR_PKT_HDR_LEN + payload_len - 1] */
    uint8_t payload_len = data[21];               /* payload_len field        */
    uint8_t crc_off     = (uint8_t)(RIVR_PKT_HDR_LEN + payload_len);
    if ((uint16_t)(crc_off + RIVR_PKT_CRC_LEN) <= (uint16_t)len) {
        uint16_t crc        = protocol_crc16(data, crc_off);
        data[crc_off]       = (uint8_t)(crc & 0xFFu);
        data[crc_off + 1u]  = (uint8_t)(crc >>   8u);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void retry_table_init(retry_table_t *rt)
{
    if (!rt) return;
    memset(rt, 0, sizeof(*rt));
}

bool retry_table_enqueue(retry_table_t *rt,
                         uint32_t ack_src_id, uint16_t cur_pkt_id,
                         uint32_t dst_id, uint32_t next_hop,
                         const uint8_t *data, uint8_t len,
                         uint32_t toa_us, uint32_t now_ms)
{
    if (!rt || !data || len == 0u) return false;

    for (uint8_t i = 0u; i < RETRY_TABLE_SIZE; i++) {
        retry_entry_t *e = &rt->entries[i];
        if (e->valid) continue;

        e->ack_src_id          = ack_src_id;
        e->cur_pkt_id          = cur_pkt_id;
        e->dst_id              = dst_id;
        e->next_hop            = next_hop;
        e->len                 = len;
        e->toa_us              = toa_us;
        e->next_retry_ms       = now_ms + RETRY_TIMEOUT_MS;
        e->timeout_interval_ms = RETRY_TIMEOUT_MS;
        e->retries_left        = RETRY_MAX;
        e->valid               = true;
        memcpy(e->data, data, len);

        if (rt->count < RETRY_TABLE_SIZE) rt->count++;
        return true;
    }

    /* Table full */
    g_rivr_metrics.retry_fail_total++;
    RIVR_LOGW(TAG, "[RETRY] table full – skipped dst=0x%08lx pkt_id=0x%04x",
              (unsigned long)dst_id, (unsigned)cur_pkt_id);
    return false;
}

bool retry_table_ack(retry_table_t *rt,
                     uint32_t ack_src_id, uint16_t ack_pkt_id)
{
    if (!rt) return false;

    for (uint8_t i = 0u; i < RETRY_TABLE_SIZE; i++) {
        retry_entry_t *e = &rt->entries[i];
        if (!e->valid) continue;
        if (e->ack_src_id == ack_src_id && e->cur_pkt_id == ack_pkt_id) {
            memset(e, 0, sizeof(*e));
            if (rt->count > 0u) rt->count--;
            return true;
        }
    }
    return false;
}

uint8_t retry_table_tick(retry_table_t *rt,
                         rb_t          *tx_queue,
                         uint32_t      *pkt_id_counter,
                         uint32_t       now_ms)
{
    if (!rt || !tx_queue || !pkt_id_counter) return 0u;

    uint8_t pushed = 0u;

    for (uint8_t i = 0u; i < RETRY_TABLE_SIZE; i++) {
        retry_entry_t *e = &rt->entries[i];
        if (!e->valid) continue;

        /* Unsigned wraparound-safe timeout comparison */
        if ((int32_t)(now_ms - e->next_retry_ms) < 0) continue;  /* not yet */

        if (e->retries_left > 0u) {
            /* ── Re-transmit with fresh pkt_id ──────────────────────────── */
            uint16_t new_pkt_id = (uint16_t)(++(*pkt_id_counter));
            e->cur_pkt_id = new_pkt_id;
            patch_pkt_id_and_crc(e->data, e->len, new_pkt_id);

            rf_tx_request_t req;
            memset(&req, 0, sizeof(req));
            memcpy(req.data, e->data, e->len);
            req.len    = e->len;
            req.toa_us = e->toa_us;
            req.due_ms = 0u;

            if (rb_try_push(tx_queue, &req)) {
                pushed++;
                g_rivr_metrics.retry_attempt_total++;
                RIVR_LOGW(TAG,
                    "[RETRY] attempt=%u/%u pkt_id=0x%04x dst=0x%08lx",
                    (unsigned)(RETRY_MAX - e->retries_left + 1u),
                    (unsigned)RETRY_MAX,
                    (unsigned)new_pkt_id,
                    (unsigned long)e->dst_id);
            }
            e->retries_left--;
            e->timeout_interval_ms <<= 1u;  /* exponential backoff */
            e->next_retry_ms = now_ms + e->timeout_interval_ms;

        } else {
            /* ── All retries exhausted — emit fallback flood ─────────────── */
            g_rivr_metrics.retry_fail_total++;
            RIVR_LOGW(TAG,
                "[RETRY] failed=%u dst=0x%08lx – initiating fallback flood",
                (unsigned)RETRY_MAX, (unsigned long)e->dst_id);

            /* Build fallback flood by patching the stored frame's wire bytes.
             * Field offsets per protocol.h wire layout:
             *   [4]     flags      [5]  ttl       [6]   hop
             *   [13–16] dst_id LE  [22] loop_guard */
            uint16_t fb_pkt_id = (uint16_t)(++(*pkt_id_counter));
            rf_tx_request_t fb;
            memset(&fb, 0u, sizeof(fb));
            memcpy(fb.data, e->data, e->len);
            fb.len    = e->len;
            fb.toa_us = e->toa_us;
            fb.due_ms = 0u;

            /* flags: clear ACK_REQ, set FALLBACK */
            fb.data[4]  = (uint8_t)((fb.data[4] & ~(uint8_t)PKT_FLAG_ACK_REQ)
                                    | (uint8_t)PKT_FLAG_FALLBACK);
            /* ttl = fallback TTL */
            fb.data[5]  = RIVR_FALLBACK_TTL;
            /* hop = 0 (originated, not a relay) */
            fb.data[6]  = 0u;
            /* dst_id = 0 (broadcast) */
            fb.data[13] = 0u;  fb.data[14] = 0u;
            fb.data[15] = 0u;  fb.data[16] = 0u;
            /* loop_guard = 0 for fresh flood */
            fb.data[LOOP_GUARD_BYTE_OFFSET] = 0u;
            /* patch pkt_id + recompute CRC */
            patch_pkt_id_and_crc(fb.data, fb.len, fb_pkt_id);

            if (rb_try_push(tx_queue, &fb)) {
                pushed++;
                g_rivr_metrics.retry_fallback_total++;
                RIVR_LOGW(TAG,
                    "[FLOOD_FALLBACK] pkt_id=0x%04x dst=0x%08lx reason=retry_exhausted",
                    (unsigned)fb_pkt_id, (unsigned long)e->dst_id);
            }

            memset(e, 0, sizeof(*e));
            if (rt->count > 0u) rt->count--;
        }
    }

    return pushed;
}

uint8_t retry_table_count(const retry_table_t *rt)
{
    if (!rt) return 0u;
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < RETRY_TABLE_SIZE; i++) {
        if (rt->entries[i].valid) n++;
    }
    return n;
}
