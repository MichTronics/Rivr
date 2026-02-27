/**
 * @file  pending_queue.c
 * @brief Phase-D pending-message queue implementation.
 *
 * Stores outgoing wire frames that cannot be sent yet because no route is
 * cached for their destination.  When a PKT_ROUTE_RPL arrives, the caller
 * invokes pending_queue_drain_for_dst() to dequeue and re-encode all saved
 * frames as unicast hops via the resolved next_hop.
 *
 * All memory is BSS.  No heap allocations.
 */

#include "pending_queue.h"
#include "protocol.h"
#include "rivr_metrics.h"
#include "rivr_log.h"
#include <string.h>

#define TAG "PQ"

void pending_queue_init(pending_queue_t *pq)
{
    if (!pq) return;
    memset(pq, 0, sizeof(*pq));
}

bool pending_queue_enqueue(pending_queue_t *pq,
                           uint32_t         dst_id,
                           const uint8_t   *data,
                           uint8_t          len,
                           uint32_t         toa_us,
                           uint32_t         now_ms)
{
    if (!pq || !data || len == 0u || dst_id == 0u) return false;

    /* Reclaim stale entries before looking for a free slot */
    pending_queue_expire(pq, now_ms);

    for (uint8_t i = 0u; i < PENDING_QUEUE_SIZE; i++) {
        pending_entry_t *e = &pq->entries[i];
        if (e->valid) continue;

        e->dst_id      = dst_id;
        e->len         = (len < RF_MAX_PAYLOAD_LEN) ? len : RF_MAX_PAYLOAD_LEN;
        e->toa_us      = toa_us;
        e->enqueued_ms = now_ms;
        e->valid       = true;
        memcpy(e->data, data, e->len);
        if (pq->count < PENDING_QUEUE_SIZE) pq->count++;
        /* Update peak gauge */
        if (pq->count > g_rivr_metrics.pq_peak) {
            g_rivr_metrics.pq_peak = pq->count;
        }
        return true;
    }
    /* Queue full even after expiry — drop and count */
    g_rivr_metrics.pq_dropped++;
    {
        uint32_t n = g_rivr_metrics.pq_dropped;
        if ((n & (n - 1u)) == 0u) {   /* log on 1,2,4,8,… */
            RIVR_LOGW(TAG, "pending queue full – frame dropped (total=%" PRIu32 ")", n);
        }
    }
    return false;  /* queue full even after expiry */
}

uint8_t pending_queue_drain_for_dst(pending_queue_t *pq,
                                     uint32_t         dst_id,
                                     uint32_t         next_hop,
                                     rb_t            *tx_queue,
                                     uint32_t         now_ms)
{
    if (!pq || dst_id == 0u || next_hop == 0u || !tx_queue) return 0u;

    uint8_t sent = 0u;

    for (uint8_t i = 0u; i < PENDING_QUEUE_SIZE; i++) {
        pending_entry_t *e = &pq->entries[i];
        if (!e->valid || e->dst_id != dst_id) continue;

        /* Evict if expired regardless of whether we manage to TX */
        bool expired = (now_ms - e->enqueued_ms) > PENDING_EXPIRY_MS;

        if (!expired) {
            /* Decode the saved wire frame, rewrite for unicast, re-encode. */
            rivr_pkt_hdr_t hdr;
            const uint8_t *pl  = NULL;
            if (protocol_decode(e->data, e->len, &hdr, &pl)) {
                hdr.dst_id = next_hop;
                hdr.ttl    = 1u;          /* single hop to next_hop */

                rf_tx_request_t req;
                memset(&req, 0, sizeof(req));
                req.due_ms = 0u;           /* drain immediately */
                req.toa_us = e->toa_us;

                int enc = protocol_encode(&hdr, pl, hdr.payload_len,
                                          req.data, sizeof(req.data));
                if (enc > 0) {
                    req.len = (uint8_t)enc;
                    if (rb_try_push(tx_queue, &req)) sent++;
                }
            }
        }

        /* Always clear the slot immediately — before any further processing.
         * This prevents a second ROUTE_RPL from draining the same entry
         * (double-send protection).  We intentionally do not retry on TX    *
         * push failure; the ROUTE_REQ will be re-issued by the next          *
         * origination attempt if the message is still needed.               */
        memset(e, 0, sizeof(*e));
        if (pq->count > 0u) pq->count--;
    }

    return sent;
}

uint8_t pending_queue_expire(pending_queue_t *pq, uint32_t now_ms)
{
    if (!pq) return 0u;
    uint8_t evicted = 0u;

    for (uint8_t i = 0u; i < PENDING_QUEUE_SIZE; i++) {
        pending_entry_t *e = &pq->entries[i];
        if (!e->valid) continue;
        if ((now_ms - e->enqueued_ms) > PENDING_EXPIRY_MS) {
            memset(e, 0, sizeof(*e));
            if (pq->count > 0u) pq->count--;
            evicted++;
        }
    }
    if (evicted > 0u) {
        g_rivr_metrics.pq_expired += evicted;
    }
    return evicted;
}

uint8_t pending_queue_count(const pending_queue_t *pq)
{
    if (!pq) return 0u;
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < PENDING_QUEUE_SIZE; i++) {
        if (pq->entries[i].valid) n++;
    }
    return n;
}
