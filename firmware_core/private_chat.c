/**
 * @file  private_chat.c
 * @brief RIVR 1-to-1 private chat engine implementation.
 *
 * All state is BSS-resident; no heap allocation anywhere in this file.
 *
 * INTEGRATION POINTS
 * ──────────────────
 * Call from the main loop (sources_rx_drain or equivalent):
 *   • private_chat_on_rx()     when pkt_type == PKT_PRIVATE_CHAT
 *   • private_chat_on_receipt() when pkt_type == PKT_DELIVERY_RECEIPT
 *   • private_chat_tick(now_ms) from the main loop tick (~100 ms)
 *
 * Call from the BLE/USB companion command handler:
 *   • private_chat_send(peer, body, body_len, &msg_id)
 *
 * Call from retry_table exhaustion hook:
 *   • private_chat_on_wire_ack(pkt_id, src_id, false)
 *
 * Call from ACK reception path:
 *   • private_chat_on_wire_ack(pkt_id, src_id, true)
 */

#include "private_chat.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "protocol.h"
#include "routing.h"
#include "route_cache.h"
#include "pending_queue.h"
#include "retry_table.h"
#include "rivr_metrics.h"
#include "rivr_log.h"
#include "timebase.h"

/* Companion push hooks (stubs compile to no-ops when BLE disabled). */
#include "ble/rivr_ble_companion.h"

#define TAG "PCHAT"

/* ── Externally visible globals (see private_chat.h) ────────────────────── */

pchat_entry_t      g_pchat_queue[PRIVATE_CHAT_QUEUE_SIZE];
pchat_dedup_entry_t g_pchat_dedup[PRIVATE_CHAT_DEDUP_SIZE];

/* ── Internal globals ────────────────────────────────────────────────────── */

/** Per-sender sequence counter — incremented on each new message.  Not
 *  persisted across reboot in this implementation.  Resets to 1 on init. */
static uint32_t s_sender_seq = 0u;

/** Receipt rate-limiter state (global, not per-peer — bounded simply). */
static pchat_receipt_rate_t s_receipt_rate = {0u, 0u};

/* ── Exported node ID (defined in main.c, main_linux.c) ─────────────────── */
extern uint32_t g_my_node_id;

/* ── Pending-queue integration (from pending_queue.c) ───────────────────── */
extern pending_queue_t g_pending_queue;

/* ── Retry table (from retry_table.c) ────────────────────────────────────── */
extern retry_table_t g_retry_table;

/* ── Route cache (from route_cache.c) ────────────────────────────────────── */
extern route_cache_t g_route_cache;

/* ── rf_tx_queue ring buffer (from test_stubs.c or radio_sx1262.c) ─────── */
extern rb_t rf_tx_queue;

/* ── Control-plane sequence counter (from rivr_embed.c) ─────────────────── */
extern uint32_t g_ctrl_seq;

/* ─────────────────────────────────────────────────────────────────────────
 *  PRIVATE HELPERS
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Derive a deterministic msg_id from the local node ID and sender_seq.
 * This makes the ID stable even if the sequence counter is the only counter
 * available (no hardware RNG required).
 *
 * Layout: [node_id:32 | sender_seq:32]
 * Unlike a random ID, this allows correlation in diagnostics (sender obvious
 * from upper 32 bits).  Uniqueness is ensured by monotone sender_seq.
 */
static uint64_t make_msg_id(uint32_t node_id, uint32_t seq)
{
    return ((uint64_t)node_id << 32) | (uint64_t)seq;
}

/** Find a free outgoing queue slot. Returns NULL if queue is full. */
static pchat_entry_t *pchat_alloc_slot(void)
{
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        if (!g_pchat_queue[i].valid) {
            memset(&g_pchat_queue[i], 0, sizeof(g_pchat_queue[i]));
            g_pchat_queue[i].valid = true;
            return &g_pchat_queue[i];
        }
    }
    return NULL;
}

/** Find an outgoing queue slot by pkt_id (for wire-ACK correlation). */
static pchat_entry_t *pchat_find_by_pkt_id(uint16_t pkt_id, uint32_t src_id)
{
    if (src_id != g_my_node_id) {
        return NULL;
    }
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        if (g_pchat_queue[i].valid && g_pchat_queue[i].pkt_id == pkt_id) {
            return &g_pchat_queue[i];
        }
    }
    return NULL;
}

/** Find an outgoing queue slot by msg_id (for receipt correlation). */
static pchat_entry_t *pchat_find_by_msg_id(uint64_t msg_id, uint32_t peer_id)
{
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        if (g_pchat_queue[i].valid
            && g_pchat_queue[i].msg_id == msg_id
            && g_pchat_queue[i].peer_id == peer_id) {
            return &g_pchat_queue[i];
        }
    }
    return NULL;
}

/** Release an outgoing queue slot and announce state to companion. */
static void pchat_release(pchat_entry_t *e)
{
    if (!e || !e->valid) {
        return;
    }
    e->valid = false;
}

/**
 * Emit a state-change notification to the connected companion app.
 * Uses the BLE CP push path (companion protocol 0x89 = PRIVATE_CHAT_STATE).
 *
 * Payload: [msg_id:8 LE][peer_id:4 LE][state:1]
 */
static void pchat_notify_state(const pchat_entry_t *e)
{
    if (!e) return;

    /* Bitmask: only notify for terminal or notable state changes. */
    switch (e->state) {
        case PCHAT_STATE_SENT:
        case PCHAT_STATE_FORWARDED:
        case PCHAT_STATE_DELIVERED:
        case PCHAT_STATE_DELIVERY_UNCONFIRMED:
        case PCHAT_STATE_FAILED_NO_ROUTE:
        case PCHAT_STATE_FAILED_RETRY:
        case PCHAT_STATE_EXPIRED:
            break;
        default:
            return;
    }

    rivr_ble_companion_push_pchat_state(e->msg_id, e->peer_id, (uint8_t)e->state);
}

/**
 * Push a received private chat event to the companion app.
 * Payload: [msg_id:8][from:4][timestamp_s:4][flags:2][body_len:1][body:N]
 */
static void pchat_notify_rx(const rivr_pkt_hdr_t *hdr,
                             const private_chat_payload_t *p)
{
    rivr_ble_companion_push_private_chat_rx(
        p->msg_id,
        hdr->src_id,
        hdr->dst_id,
        p->sender_seq,
        p->timestamp_s,
        p->flags,
        p->body,
        p->body_len);
}

/**
 * Push a delivery receipt event to the companion app.
 */
static void pchat_notify_receipt(const delivery_receipt_payload_t *r)
{
    rivr_ble_companion_push_delivery_receipt(
        r->orig_msg_id,
        r->sender_id,
        r->timestamp_s,
        r->status);
}

/**
 * Add a received msg_id to the dedup cache.
 * Oldest entry is evicted when the cache is full (LRU eviction by seen_ms).
 */
static void pchat_dedup_add(uint64_t msg_id, uint32_t from_id, uint32_t now_ms)
{
    /* Find a free slot or the oldest occupied slot. */
    uint8_t target = 0u;
    uint32_t oldest_ms = now_ms;

    for (uint8_t i = 0u; i < PRIVATE_CHAT_DEDUP_SIZE; i++) {
        if (!g_pchat_dedup[i].valid) {
            target = i;
            goto found;
        }
        if (g_pchat_dedup[i].seen_ms < oldest_ms) {
            oldest_ms = g_pchat_dedup[i].seen_ms;
            target = i;
        }
    }

found:
    g_pchat_dedup[target].msg_id   = msg_id;
    g_pchat_dedup[target].from_id  = from_id;
    g_pchat_dedup[target].seen_ms  = now_ms;
    g_pchat_dedup[target].valid    = true;
}

/**
 * Return true if msg_id from from_id is already in the dedup cache.
 */
static bool pchat_dedup_check(uint64_t msg_id, uint32_t from_id)
{
    for (uint8_t i = 0u; i < PRIVATE_CHAT_DEDUP_SIZE; i++) {
        if (g_pchat_dedup[i].valid
            && g_pchat_dedup[i].msg_id == msg_id
            && g_pchat_dedup[i].from_id == from_id) {
            return true;
        }
    }
    return false;
}

/**
 * Check and update receipt rate limiter.
 * Returns true if a receipt may be sent, false if rate-limited.
 */
static bool pchat_receipt_rate_ok(uint32_t now_ms)
{
    if ((now_ms - s_receipt_rate.window_start_ms) >= PRIVATE_CHAT_RECEIPT_RATE_WIN_MS) {
        s_receipt_rate.window_start_ms = now_ms;
        s_receipt_rate.count = 0u;
    }
    if (s_receipt_rate.count >= PRIVATE_CHAT_RECEIPT_RATE_MAX) {
        return false;
    }
    s_receipt_rate.count++;
    return true;
}

/**
 * Encode a delivery receipt and push it to the TX queue.
 */
static void pchat_emit_receipt(uint32_t to_node_id,
                                uint64_t orig_msg_id,
                                uint32_t sender_id,
                                uint8_t status,
                                uint32_t now_s)
{
    delivery_receipt_payload_t r;
    r.orig_msg_id  = orig_msg_id;
    r.sender_id    = sender_id;
    r.timestamp_s  = now_s;
    r.status       = status;

    uint8_t pay_buf[DELIVERY_RECEIPT_PAYLOAD_LEN];
    int pay_len = private_chat_encode_receipt(&r, pay_buf, sizeof(pay_buf));
    if (pay_len < 0) {
        RIVR_LOGW(TAG, "receipt encode fail");
        g_rivr_metrics.private_chat_invalid_total++;
        return;
    }

    /* Build wire frame. */
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_DELIVERY_RECEIPT;
    hdr.flags       = PKT_FLAG_ACK_REQ;
    hdr.ttl         = RIVR_PKT_DEFAULT_TTL;
    hdr.hop         = 0u;
    hdr.net_id      = 0u; /* filled by caller context if needed */
    hdr.src_id      = g_my_node_id;
    hdr.dst_id      = to_node_id;
    hdr.seq         = 0u;
    hdr.pkt_id      = (uint16_t)++g_ctrl_seq;
    hdr.payload_len = (uint8_t)pay_len;
    hdr.loop_guard  = 0u;

    /* Look up route. */
    const route_cache_entry_t *route = route_cache_lookup(&g_route_cache,
                                                            to_node_id,
                                                            tb_millis());
    if (route) {
        hdr.dst_id = route->next_hop;
        hdr.ttl    = 1u; /* unicast — one hop to next relay */
    }

    uint8_t frame[255];
    int frame_len = protocol_encode(&hdr, pay_buf, (uint8_t)pay_len,
                                    frame, sizeof(frame));
    if (frame_len < 0) {
        RIVR_LOGW(TAG, "receipt frame encode fail");
        return;
    }

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.toa_us    = 0u; /* approximated downstream */
    req.due_ms    = 0u; /* send as soon as possible */
    req.len       = (uint8_t)frame_len;
    memcpy(req.data, frame, (size_t)frame_len);

    if (!rb_try_push(&rf_tx_queue, &req)) {
        RIVR_LOGW(TAG, "receipt TX queue full");
        g_rivr_metrics.tx_queue_full++;
    } else {
        g_rivr_metrics.private_chat_receipt_tx_total++;
    }
}

/**
 * Attempt to transmit an outgoing private chat message.
 * Looks up route; if found, builds unicast frame and pushes to TX queue.
 * Returns true if frame pushed, false if no route (caller should await route).
 */
static bool pchat_try_send_frame(pchat_entry_t *e, uint32_t now_ms)
{
    if (!e || !e->valid) {
        return false;
    }

    const route_cache_entry_t *route = route_cache_lookup(&g_route_cache,
                                                            e->peer_id,
                                                            now_ms);
    uint32_t next_hop = e->peer_id;
    uint8_t  ttl      = RIVR_PKT_DEFAULT_TTL;

    if (route) {
        next_hop = route->next_hop;
        ttl = 1u;
        g_rivr_metrics.route_cache_hit_total++;
    } else {
        g_rivr_metrics.route_cache_miss_total++;

        /* Issue route discovery and park in pending queue. */
        uint8_t req_buf[255];
        ++g_ctrl_seq;
        int req_frame_len = routing_build_route_req(g_my_node_id, e->peer_id,
                                                     (uint16_t)g_ctrl_seq,
                                                     (uint16_t)g_ctrl_seq,
                                                     req_buf, sizeof(req_buf));
        if (req_frame_len > 0) {
            rf_tx_request_t rreq;
            memset(&rreq, 0, sizeof(rreq));
            rreq.len = (uint8_t)req_frame_len;
            rreq.due_ms = 0u;
            memcpy(rreq.data, req_buf, (size_t)req_frame_len);
            (void)rb_try_push(&rf_tx_queue, &rreq);
        }

        /* Park the frame in the pending queue. */
        if (!pending_queue_enqueue(&g_pending_queue, e->peer_id,
                                    e->frame, e->frame_len, 0u, now_ms)) {
            g_rivr_metrics.pq_dropped++;
        }

        e->state = PCHAT_STATE_AWAITING_ROUTE;
        pchat_notify_state(e);
        return false;
    }

    /* Route known: patch dst_id → next_hop and update TTL in the stored frame,
     * then push. */
    rivr_pkt_hdr_t hdr;
    uint8_t        pay_buf[PRIVATE_CHAT_MAX_PAYLOAD_LEN];
    uint8_t        pay_len = 0u;

    /* Re-decode from stored frame to get payload bytes. */
    if (e->frame_len > RIVR_PKT_HDR_LEN + RIVR_PKT_CRC_LEN) {
        pay_len = e->frame_len - RIVR_PKT_HDR_LEN - RIVR_PKT_CRC_LEN;
        if (pay_len <= sizeof(pay_buf)) {
            memcpy(pay_buf,
                   e->frame + RIVR_PKT_HDR_LEN,
                   pay_len);
        } else {
            RIVR_LOGW(TAG, "stored frame payload too long");
            return false;
        }
    }

    /* Re-decode header bytes manually (protocol_decode not needed here since
     * we trust our own stored frame; just patch the fields that may change). */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_PRIVATE_CHAT;
    hdr.flags       = PKT_FLAG_ACK_REQ;
    hdr.ttl         = ttl;
    hdr.hop         = 0u;
    hdr.net_id      = 0u;
    hdr.src_id      = g_my_node_id;
    hdr.dst_id      = next_hop;
    hdr.seq         = (uint16_t)(e->msg_id & 0xFFFFu); /* low 16 bits for ordering */
    hdr.pkt_id      = (uint16_t)++g_ctrl_seq;
    hdr.payload_len = pay_len;
    hdr.loop_guard  = 0u;

    e->pkt_id = hdr.pkt_id;  /* remember for wire-ACK correlation */

    uint8_t new_frame[255];
    int new_len = protocol_encode(&hdr, pay_buf, pay_len, new_frame, sizeof(new_frame));
    if (new_len < 0) {
        RIVR_LOGW(TAG, "frame re-encode fail");
        return false;
    }

    /* Update stored frame for retransmit support. */
    memcpy(e->frame, new_frame, (size_t)new_len);
    e->frame_len = (uint8_t)new_len;

    /* Push to retry table for wire-level reliability. */
    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    req.len      = (uint8_t)new_len;
    req.due_ms   = 0u;
    memcpy(req.data, new_frame, (size_t)new_len);

    /* First transmission: push to TX queue immediately. */
    if (!rb_try_push(&rf_tx_queue, &req)) {
        g_rivr_metrics.tx_queue_full++;
        return false;
    }

    /* Register in retry table for wire-level ACK reliability. */
    (void)retry_table_enqueue(&g_retry_table,
                               g_my_node_id, hdr.pkt_id,
                               e->peer_id, next_hop,
                               new_frame, (uint8_t)new_len,
                               0u, now_ms);

    if (e->sent_ms == 0u) {
        e->sent_ms = now_ms;
    }
    e->state = PCHAT_STATE_SENT;
    g_rivr_metrics.private_chat_tx_total++;
    pchat_notify_state(e);
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 *  PUBLIC API
 * ───────────────────────────────────────────────────────────────────────── */

void private_chat_init(void)
{
    memset(g_pchat_queue, 0, sizeof(g_pchat_queue));
    memset(g_pchat_dedup, 0, sizeof(g_pchat_dedup));
    memset(&s_receipt_rate, 0, sizeof(s_receipt_rate));
    s_sender_seq = 1u;
}

pchat_error_t private_chat_send(uint32_t peer,
                                 const uint8_t *body,
                                 uint8_t body_len,
                                 uint64_t *out_msg_id)
{
    if (peer == 0u || peer == 0xFFFFFFFFu) {
        return PCHAT_ERR_INVALID_PEER;
    }
    if (body_len > PRIVATE_CHAT_MAX_BODY) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_BODY_TOO_LONG;
    }
    if (!body && body_len > 0u) {
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    pchat_entry_t *e = pchat_alloc_slot();
    if (!e) {
        g_rivr_metrics.tx_queue_full++;
        return PCHAT_ERR_QUEUE_FULL;
    }

    uint32_t now_ms = tb_millis();
    uint32_t now_s  = now_ms / 1000u; /* coarse; good enough for timestamp */

    uint32_t seq = s_sender_seq++;
    uint64_t msg_id = make_msg_id(g_my_node_id, seq);

    if (out_msg_id) {
        *out_msg_id = msg_id;
    }

    /* Build payload. */
    private_chat_payload_t p;
    memset(&p, 0, sizeof(p));
    p.msg_id          = msg_id;
    p.sender_seq      = seq;
    p.timestamp_s     = now_s;
    p.flags           = PCHAT_FLAGS_DEFAULT;
    p.expires_delta_s = (uint16_t)(PRIVATE_CHAT_QUEUE_EXPIRY_MS / 1000u);
    p.reply_to_msg_id = 0u;
    p.body_len        = body_len;
    if (body_len > 0u) {
        memcpy(p.body, body, body_len);
    }

    uint8_t pay_buf[PRIVATE_CHAT_MAX_PAYLOAD_LEN];
    int pay_len = private_chat_encode_payload(&p, pay_buf, sizeof(pay_buf));
    if (pay_len < 0) {
        e->valid = false;
        return PCHAT_ERR_ENCODE_FAIL;
    }

    /* Build tentative wire frame (dst may be replaced once route is resolved). */
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = PKT_PRIVATE_CHAT;
    hdr.flags       = PKT_FLAG_ACK_REQ;
    hdr.ttl         = RIVR_PKT_DEFAULT_TTL;
    hdr.hop         = 0u;
    hdr.net_id      = 0u;
    hdr.src_id      = g_my_node_id;
    hdr.dst_id      = peer;          /* will be replaced with next_hop if route found */
    hdr.seq         = (uint16_t)(seq & 0xFFFFu);
    hdr.pkt_id      = (uint16_t)++g_ctrl_seq;
    hdr.payload_len = (uint8_t)pay_len;
    hdr.loop_guard  = 0u;

    int frame_len = protocol_encode(&hdr, pay_buf, (uint8_t)pay_len,
                                    e->frame, sizeof(e->frame));
    if (frame_len < 0) {
        e->valid = false;
        return PCHAT_ERR_ENCODE_FAIL;
    }

    e->msg_id       = msg_id;
    e->peer_id      = peer;
    e->frame_len    = (uint8_t)frame_len;
    e->pkt_id       = hdr.pkt_id;
    e->enqueued_ms  = now_ms;
    e->sent_ms      = 0u;
    e->state        = PCHAT_STATE_QUEUED;

    /* Try to send immediately; may transition to AWAITING_ROUTE. */
    (void)pchat_try_send_frame(e, now_ms);

    return PCHAT_OK;
}

void private_chat_on_wire_ack(uint16_t pkt_id, uint32_t src_id, bool success)
{
    pchat_entry_t *e = pchat_find_by_pkt_id(pkt_id, src_id);
    if (!e) {
        return;
    }

    if (success) {
        if (e->state == PCHAT_STATE_SENT) {
            e->state = PCHAT_STATE_FORWARDED;
            g_rivr_metrics.ack_rx_total++;
            pchat_notify_state(e);
        }
    } else {
        /* Retry budget exhausted. */
        if (e->state == PCHAT_STATE_SENT || e->state == PCHAT_STATE_FORWARDED) {
            e->state = PCHAT_STATE_FAILED_RETRY;
            g_rivr_metrics.private_chat_failed_retry_budget_total++;
            pchat_notify_state(e);
            pchat_release(e);
        }
    }
}

pchat_error_t private_chat_on_rx(const rivr_pkt_hdr_t *hdr,
                                  const uint8_t *payload,
                                  uint8_t pay_len)
{
    if (!hdr || !payload) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    /* Destination validation — must be for us specifically. */
    if (hdr->dst_id != g_my_node_id) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_DST_MISMATCH;
    }

    /* Minimum payload size check. */
    if (pay_len < PRIVATE_CHAT_PAYLOAD_HDR_LEN) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    /* Decode payload. */
    private_chat_payload_t p;
    if (private_chat_decode_payload(payload, pay_len, &p) != PCHAT_OK) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    /* Body length coherence check. */
    if (p.body_len > PRIVATE_CHAT_MAX_BODY
        || (uint16_t)pay_len < (uint16_t)(PRIVATE_CHAT_PAYLOAD_HDR_LEN + p.body_len)) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    uint32_t now_ms = tb_millis();

    /* Dedup check — silently drop duplicates. */
    if (pchat_dedup_check(p.msg_id, hdr->src_id)) {
        g_rivr_metrics.private_chat_dedup_drop_total++;
        return PCHAT_ERR_DUPLICATE;
    }
    pchat_dedup_add(p.msg_id, hdr->src_id, now_ms);

    /* Expiry check (if sender clock is available). */
    if (p.expires_delta_s > 0u && p.timestamp_s > 0u) {
        uint32_t now_s = now_ms / 1000u;
        uint32_t expiry_s = p.timestamp_s + p.expires_delta_s;
        if (now_s > expiry_s + 120u) { /* 120 s clock-skew tolerance */
            g_rivr_metrics.private_chat_expired_total++;
            if (p.flags & PCHAT_FLAG_RECEIPT_REQ) {
                if (pchat_receipt_rate_ok(now_ms)) {
                    pchat_emit_receipt(hdr->src_id, p.msg_id, g_my_node_id,
                                       RCPT_STATUS_EXPIRED, now_ms / 1000u);
                }
            }
            return PCHAT_ERR_INVALID_PAYLOAD; /* expired before delivery */
        }
    }

    g_rivr_metrics.private_chat_rx_total++;
    RIVR_LOGI(TAG, "rx msg_id=0x%016" PRIx64 " from=0x%08" PRIx32
                  " body_len=%u", p.msg_id, hdr->src_id, p.body_len);

    /* Notify companion. */
    pchat_notify_rx(hdr, &p);

    /* Emit delivery receipt if requested and rate-limit permits. */
    if (p.flags & PCHAT_FLAG_RECEIPT_REQ) {
        if (pchat_receipt_rate_ok(now_ms)) {
            pchat_emit_receipt(hdr->src_id, p.msg_id, g_my_node_id,
                               RCPT_STATUS_DELIVERED, now_ms / 1000u);
            g_rivr_metrics.private_chat_receipt_tx_total++;
        } else {
            RIVR_LOGW(TAG, "receipt rate-limited for msg 0x%016" PRIx64, p.msg_id);
        }
    }

    return PCHAT_OK;
}

pchat_error_t private_chat_on_receipt(const rivr_pkt_hdr_t *hdr,
                                       const uint8_t *payload,
                                       uint8_t pay_len)
{
    if (!hdr || !payload
        || pay_len != DELIVERY_RECEIPT_PAYLOAD_LEN) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    /* Receipt must be addressed to us. */
    if (hdr->dst_id != g_my_node_id) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_DST_MISMATCH;
    }

    delivery_receipt_payload_t r;
    if (private_chat_decode_receipt(payload, pay_len, &r) != PCHAT_OK) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    /* sender_id in receipt should match our local node (we sent the original). */
    if (r.sender_id != g_my_node_id) {
        g_rivr_metrics.private_chat_invalid_total++;
        return PCHAT_ERR_DST_MISMATCH;
    }

    g_rivr_metrics.private_chat_receipt_rx_total++;

    pchat_entry_t *e = pchat_find_by_msg_id(r.orig_msg_id, hdr->src_id);
    if (!e) {
        /* Receipt for unknown / already-cleared message — not an error,
         * just a no-op (common after reboot). */
        RIVR_LOGD(TAG, "receipt for unknown msg 0x%016" PRIx64, r.orig_msg_id);
        pchat_notify_receipt(&r);
        return PCHAT_OK;
    }

    /* Transition to final state. */
    switch (r.status) {
        case RCPT_STATUS_DELIVERED:
            e->state = PCHAT_STATE_DELIVERED;
            break;
        case RCPT_STATUS_REJECTED:
        case RCPT_STATUS_EXPIRED:
        case RCPT_STATUS_UNSUPPORTED:
        default:
            e->state = PCHAT_STATE_FAILED_RETRY; /* terminal failure */
            g_rivr_metrics.private_chat_failed_retry_budget_total++;
            break;
    }

    RIVR_LOGI(TAG, "receipt for 0x%016" PRIx64 " status=%u",
                  r.orig_msg_id, r.status);

    pchat_notify_state(e);
    pchat_notify_receipt(&r);
    pchat_release(e);

    return PCHAT_OK;
}

void private_chat_tick(uint32_t now_ms)
{
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        pchat_entry_t *e = &g_pchat_queue[i];
        if (!e->valid) {
            continue;
        }

        uint32_t age_ms = now_ms - e->enqueued_ms;

        /* ── Hard expiry ─────────────────────────────────────────────────── */
        if (age_ms >= PRIVATE_CHAT_QUEUE_EXPIRY_MS) {
            switch (e->state) {
                case PCHAT_STATE_QUEUED:
                case PCHAT_STATE_AWAITING_ROUTE:
                    e->state = PCHAT_STATE_FAILED_NO_ROUTE;
                    g_rivr_metrics.private_chat_failed_no_route_total++;
                    break;
                case PCHAT_STATE_SENT:
                    e->state = PCHAT_STATE_EXPIRED;
                    g_rivr_metrics.private_chat_expired_total++;
                    break;
                case PCHAT_STATE_FORWARDED:
                    e->state = PCHAT_STATE_DELIVERY_UNCONFIRMED;
                    g_rivr_metrics.private_chat_receipt_timeout_total++;
                    break;
                default:
                    break;
            }
            pchat_notify_state(e);
            pchat_release(e);
            continue;
        }

        /* ── Receipt timeout (already forwarded but no receipt yet) ──────── */
        if (e->state == PCHAT_STATE_FORWARDED && e->sent_ms != 0u) {
            uint32_t since_sent = now_ms - e->sent_ms;
            if (since_sent >= PRIVATE_CHAT_RECEIPT_TMO_MS) {
                e->state = PCHAT_STATE_DELIVERY_UNCONFIRMED;
                g_rivr_metrics.private_chat_receipt_timeout_total++;
                pchat_notify_state(e);
                pchat_release(e);
                continue;
            }
        }

        /* ── Route-resolved drain — re-attempt AWAITING_ROUTE entries ────── */
        if (e->state == PCHAT_STATE_AWAITING_ROUTE) {
            const route_cache_entry_t *route =
                route_cache_lookup(&g_route_cache, e->peer_id, now_ms);
            if (route) {
                RIVR_LOGI(TAG, "route resolved for 0x%08" PRIx32
                              " -> 0x%08" PRIx32, e->peer_id, route->next_hop);
                g_rivr_metrics.pending_queue_drained_total++;
                (void)pchat_try_send_frame(e, now_ms);
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 *  ENCODE / DECODE
 * ───────────────────────────────────────────────────────────────────────── */

int private_chat_encode_payload(const private_chat_payload_t *p,
                                 uint8_t *out_buf,
                                 uint8_t out_cap)
{
    if (!p || !out_buf) {
        return -1;
    }
    if (p->body_len > PRIVATE_CHAT_MAX_BODY) {
        return -1;
    }
    uint8_t total = (uint8_t)(PRIVATE_CHAT_PAYLOAD_HDR_LEN + p->body_len);
    if (total > out_cap || total > RIVR_PKT_MAX_PAYLOAD) {
        return -1;
    }

    uint8_t *d = out_buf;
    /* msg_id — 8 bytes LE */
    d[0] = (uint8_t)(p->msg_id & 0xFFu);
    d[1] = (uint8_t)((p->msg_id >>  8) & 0xFFu);
    d[2] = (uint8_t)((p->msg_id >> 16) & 0xFFu);
    d[3] = (uint8_t)((p->msg_id >> 24) & 0xFFu);
    d[4] = (uint8_t)((p->msg_id >> 32) & 0xFFu);
    d[5] = (uint8_t)((p->msg_id >> 40) & 0xFFu);
    d[6] = (uint8_t)((p->msg_id >> 48) & 0xFFu);
    d[7] = (uint8_t)((p->msg_id >> 56) & 0xFFu);
    /* sender_seq — 4 bytes LE */
    d[8]  = (uint8_t)(p->sender_seq & 0xFFu);
    d[9]  = (uint8_t)((p->sender_seq >> 8) & 0xFFu);
    d[10] = (uint8_t)((p->sender_seq >> 16) & 0xFFu);
    d[11] = (uint8_t)((p->sender_seq >> 24) & 0xFFu);
    /* timestamp_s — 4 bytes LE */
    d[12] = (uint8_t)(p->timestamp_s & 0xFFu);
    d[13] = (uint8_t)((p->timestamp_s >> 8) & 0xFFu);
    d[14] = (uint8_t)((p->timestamp_s >> 16) & 0xFFu);
    d[15] = (uint8_t)((p->timestamp_s >> 24) & 0xFFu);
    /* flags — 2 bytes LE */
    d[16] = (uint8_t)(p->flags & 0xFFu);
    d[17] = (uint8_t)((p->flags >> 8) & 0xFFu);
    /* expires_delta_s — 2 bytes LE */
    d[18] = (uint8_t)(p->expires_delta_s & 0xFFu);
    d[19] = (uint8_t)((p->expires_delta_s >> 8) & 0xFFu);
    /* reply_to_msg_id — 8 bytes LE */
    d[20] = (uint8_t)(p->reply_to_msg_id & 0xFFu);
    d[21] = (uint8_t)((p->reply_to_msg_id >>  8) & 0xFFu);
    d[22] = (uint8_t)((p->reply_to_msg_id >> 16) & 0xFFu);
    d[23] = (uint8_t)((p->reply_to_msg_id >> 24) & 0xFFu);
    d[24] = (uint8_t)((p->reply_to_msg_id >> 32) & 0xFFu);
    d[25] = (uint8_t)((p->reply_to_msg_id >> 40) & 0xFFu);
    d[26] = (uint8_t)((p->reply_to_msg_id >> 48) & 0xFFu);
    d[27] = (uint8_t)((p->reply_to_msg_id >> 56) & 0xFFu);
    /* body_len — 1 byte */
    d[28] = p->body_len;
    /* body */
    if (p->body_len > 0u) {
        memcpy(&d[29], p->body, p->body_len);
    }

    return (int)total;
}

pchat_error_t private_chat_decode_payload(const uint8_t *buf,
                                           uint8_t len,
                                           private_chat_payload_t *out)
{
    if (!buf || !out || len < PRIVATE_CHAT_PAYLOAD_HDR_LEN) {
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    const uint8_t *d = buf;

    out->msg_id  = (uint64_t)d[0]
                 | ((uint64_t)d[1] <<  8)
                 | ((uint64_t)d[2] << 16)
                 | ((uint64_t)d[3] << 24)
                 | ((uint64_t)d[4] << 32)
                 | ((uint64_t)d[5] << 40)
                 | ((uint64_t)d[6] << 48)
                 | ((uint64_t)d[7] << 56);

    out->sender_seq = (uint32_t)d[8]
                    | ((uint32_t)d[9]  << 8)
                    | ((uint32_t)d[10] << 16)
                    | ((uint32_t)d[11] << 24);

    out->timestamp_s = (uint32_t)d[12]
                     | ((uint32_t)d[13] << 8)
                     | ((uint32_t)d[14] << 16)
                     | ((uint32_t)d[15] << 24);

    out->flags = (uint16_t)d[16] | ((uint16_t)d[17] << 8);

    out->expires_delta_s = (uint16_t)d[18] | ((uint16_t)d[19] << 8);

    out->reply_to_msg_id = (uint64_t)d[20]
                         | ((uint64_t)d[21] <<  8)
                         | ((uint64_t)d[22] << 16)
                         | ((uint64_t)d[23] << 24)
                         | ((uint64_t)d[24] << 32)
                         | ((uint64_t)d[25] << 40)
                         | ((uint64_t)d[26] << 48)
                         | ((uint64_t)d[27] << 56);

    out->body_len = d[28];

    if (out->body_len > PRIVATE_CHAT_MAX_BODY) {
        return PCHAT_ERR_INVALID_PAYLOAD;
    }
    if ((uint16_t)len < (uint16_t)(PRIVATE_CHAT_PAYLOAD_HDR_LEN + out->body_len)) {
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    memset(out->body, 0, sizeof(out->body));
    if (out->body_len > 0u) {
        memcpy(out->body, &d[PRIVATE_CHAT_PAYLOAD_HDR_LEN], out->body_len);
    }

    return PCHAT_OK;
}

int private_chat_encode_receipt(const delivery_receipt_payload_t *r,
                                 uint8_t *out_buf,
                                 uint8_t out_cap)
{
    if (!r || !out_buf || out_cap < DELIVERY_RECEIPT_PAYLOAD_LEN) {
        return -1;
    }

    uint8_t *d = out_buf;
    /* orig_msg_id — 8 bytes LE */
    d[0] = (uint8_t)(r->orig_msg_id & 0xFFu);
    d[1] = (uint8_t)((r->orig_msg_id >>  8) & 0xFFu);
    d[2] = (uint8_t)((r->orig_msg_id >> 16) & 0xFFu);
    d[3] = (uint8_t)((r->orig_msg_id >> 24) & 0xFFu);
    d[4] = (uint8_t)((r->orig_msg_id >> 32) & 0xFFu);
    d[5] = (uint8_t)((r->orig_msg_id >> 40) & 0xFFu);
    d[6] = (uint8_t)((r->orig_msg_id >> 48) & 0xFFu);
    d[7] = (uint8_t)((r->orig_msg_id >> 56) & 0xFFu);
    /* sender_id — 4 bytes LE */
    d[8]  = (uint8_t)(r->sender_id & 0xFFu);
    d[9]  = (uint8_t)((r->sender_id >> 8) & 0xFFu);
    d[10] = (uint8_t)((r->sender_id >> 16) & 0xFFu);
    d[11] = (uint8_t)((r->sender_id >> 24) & 0xFFu);
    /* timestamp_s — 4 bytes LE */
    d[12] = (uint8_t)(r->timestamp_s & 0xFFu);
    d[13] = (uint8_t)((r->timestamp_s >> 8) & 0xFFu);
    d[14] = (uint8_t)((r->timestamp_s >> 16) & 0xFFu);
    d[15] = (uint8_t)((r->timestamp_s >> 24) & 0xFFu);
    /* status — 1 byte */
    d[16] = r->status;

    return (int)DELIVERY_RECEIPT_PAYLOAD_LEN;
}

pchat_error_t private_chat_decode_receipt(const uint8_t *buf,
                                           uint8_t len,
                                           delivery_receipt_payload_t *out)
{
    if (!buf || !out || len < DELIVERY_RECEIPT_PAYLOAD_LEN) {
        return PCHAT_ERR_INVALID_PAYLOAD;
    }

    const uint8_t *d = buf;

    out->orig_msg_id = (uint64_t)d[0]
                     | ((uint64_t)d[1] <<  8)
                     | ((uint64_t)d[2] << 16)
                     | ((uint64_t)d[3] << 24)
                     | ((uint64_t)d[4] << 32)
                     | ((uint64_t)d[5] << 40)
                     | ((uint64_t)d[6] << 48)
                     | ((uint64_t)d[7] << 56);

    out->sender_id = (uint32_t)d[8]
                   | ((uint32_t)d[9]  << 8)
                   | ((uint32_t)d[10] << 16)
                   | ((uint32_t)d[11] << 24);

    out->timestamp_s = (uint32_t)d[12]
                     | ((uint32_t)d[13] << 8)
                     | ((uint32_t)d[14] << 16)
                     | ((uint32_t)d[15] << 24);

    out->status = d[16];

    return PCHAT_OK;
}

/* ─────────────────────────────────────────────────────────────────────────
 *  DIAGNOSTICS
 * ───────────────────────────────────────────────────────────────────────── */

void private_chat_print_diag(void)
{
    uint8_t active  = 0u;
    uint32_t now_ms = tb_millis();

    printf("@PCHAT_DIAG {"
           "\"queue_depth\":%u,"
           "\"tx_total\":%" PRIu32 ","
           "\"rx_total\":%" PRIu32 ","
           "\"receipt_tx\":%" PRIu32 ","
           "\"receipt_rx\":%" PRIu32 ","
           "\"retry_total\":%" PRIu32 ","
           "\"expired\":%" PRIu32 ","
           "\"failed_no_route\":%" PRIu32 ","
           "\"failed_retry\":%" PRIu32 ","
           "\"dedup_drop\":%" PRIu32 ","
           "\"invalid\":%" PRIu32 ","
           "\"receipt_timeout\":%" PRIu32 ","
           "\"max_body_bytes\":%u,"
           "\"queue_expiry_ms\":%u,"
           "\"receipt_tmo_ms\":%u",
           private_chat_queue_depth(),
           g_rivr_metrics.private_chat_tx_total,
           g_rivr_metrics.private_chat_rx_total,
           g_rivr_metrics.private_chat_receipt_tx_total,
           g_rivr_metrics.private_chat_receipt_rx_total,
           g_rivr_metrics.private_chat_retry_total,
           g_rivr_metrics.private_chat_expired_total,
           g_rivr_metrics.private_chat_failed_no_route_total,
           g_rivr_metrics.private_chat_failed_retry_budget_total,
           g_rivr_metrics.private_chat_dedup_drop_total,
           g_rivr_metrics.private_chat_invalid_total,
           g_rivr_metrics.private_chat_receipt_timeout_total,
           PRIVATE_CHAT_MAX_BODY,
           PRIVATE_CHAT_QUEUE_EXPIRY_MS,
           PRIVATE_CHAT_RECEIPT_TMO_MS);

    printf(",\"pending\":[");
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        const pchat_entry_t *e = &g_pchat_queue[i];
        if (!e->valid) {
            continue;
        }
        if (active > 0u) {
            printf(",");
        }
        printf("{\"peer\":\"0x%08" PRIx32
               "\",\"msg_id\":\"0x%016" PRIx64
               "\",\"state\":%u"
               ",\"age_ms\":%" PRIu32 "}",
               e->peer_id,
               e->msg_id,
               (unsigned)e->state,
               now_ms - e->enqueued_ms);
        active++;
    }
    printf("]}\n");
}

uint8_t private_chat_queue_depth(void)
{
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < PRIVATE_CHAT_QUEUE_SIZE; i++) {
        if (g_pchat_queue[i].valid) {
            n++;
        }
    }
    return n;
}
