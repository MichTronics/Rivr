/**
 * @file  send_queue.c
 * @brief Originated-message outbox queue implementation.
 *
 * See send_queue.h for the full design description.
 *
 * All memory lives in BSS — no heap allocations.
 */

#include "send_queue.h"
#include "rivr_metrics.h"
#include "rivr_log.h"
#include <string.h>
#include <inttypes.h>

#define TAG "SQ"

/* ── helpers ─────────────────────────────────────────────────────────────── */

/** Evict entries whose age exceeds SEND_QUEUE_EXPIRY_MS.
 *  Only evicts from the head (oldest) because we maintain FIFO order. */
static void send_queue_expire(send_queue_t *sq, uint32_t now_ms)
{
    while (sq->count > 0u) {
        send_entry_t *e = &sq->entries[sq->head];
        if (!e->valid) {
            /* Defensive: skip corrupt slot */
            sq->head = (uint8_t)((sq->head + 1u) % SEND_QUEUE_SIZE);
            if (sq->count > 0u) sq->count--;
            continue;
        }
        if ((now_ms - e->enqueued_ms) <= SEND_QUEUE_EXPIRY_MS) break;

        /* Evict this head entry */
        memset(e, 0, sizeof(*e));
        sq->head = (uint8_t)((sq->head + 1u) % SEND_QUEUE_SIZE);
        sq->count--;
        g_rivr_metrics.sq_expired++;
        RIVR_LOGW(TAG, "send_queue: expired entry evicted (count=%" PRIu8 ")", sq->count);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void send_queue_init(send_queue_t *sq)
{
    if (!sq) return;
    memset(sq, 0, sizeof(*sq));
}

bool send_queue_enqueue(send_queue_t  *sq,
                        const uint8_t *data,
                        uint8_t        len,
                        uint32_t       toa_us,
                        uint32_t       now_ms)
{
    if (!sq || !data || len == 0u) return false;

    /* Reclaim stale head entries before checking for space */
    send_queue_expire(sq, now_ms);

    if (sq->count >= SEND_QUEUE_SIZE) {
        g_rivr_metrics.sq_dropped++;
        uint32_t n = g_rivr_metrics.sq_dropped;
        if ((n & (n - 1u)) == 0u) {   /* log on 1, 2, 4, 8, … */
            RIVR_LOGW(TAG, "send_queue full – frame dropped (total=%" PRIu32 ")", n);
        }
        return false;
    }

    send_entry_t *e = &sq->entries[sq->tail];
    memcpy(e->data, data, len);
    e->len         = len;
    e->toa_us      = toa_us;
    e->enqueued_ms = now_ms;
    e->valid       = true;

    sq->tail  = (uint8_t)((sq->tail + 1u) % SEND_QUEUE_SIZE);
    sq->count++;

    /* Update peak gauge */
    if (sq->count > g_rivr_metrics.sq_peak) {
        g_rivr_metrics.sq_peak = sq->count;
    }

    RIVR_LOGD(TAG, "send_queue: enqueued len=%u count=%" PRIu8, (unsigned)len, sq->count);
    return true;
}

uint8_t send_queue_tick(send_queue_t *sq,
                        rb_t         *tx_queue,
                        uint32_t      now_ms)
{
    if (!sq || !tx_queue) return 0u;

    /* Always evict stale head entries first */
    send_queue_expire(sq, now_ms);

    if (sq->count == 0u) return 0u;

    /* Push if the TX ring-buffer has room — rb_try_push() returns false if
     * full; leave the entry in place and try again next tick. */
    send_entry_t *e = &sq->entries[sq->head];
    if (!e->valid) {
        /* Should not happen after expire(), but guard defensively */
        memset(e, 0, sizeof(*e));
        sq->head  = (uint8_t)((sq->head + 1u) % SEND_QUEUE_SIZE);
        if (sq->count > 0u) sq->count--;
        return 0u;
    }

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    memcpy(req.data, e->data, e->len);
    req.len    = e->len;
    req.toa_us = e->toa_us;
    req.due_ms = 0u;

    if (!rb_try_push(tx_queue, &req)) {
        /* Ring reported available but push failed — ring is a power-of-2
         * SPSC and leaves one slot fallow, so this can happen when only
         * 1 slot appears free.  Leave the entry in place for next tick. */
        return 0u;
    }

    /* Successfully handed off — clear the slot */
    memset(e, 0, sizeof(*e));
    sq->head  = (uint8_t)((sq->head + 1u) % SEND_QUEUE_SIZE);
    sq->count--;

    RIVR_LOGD(TAG, "send_queue: drained to tx_queue (count=%" PRIu8 ")", sq->count);
    return 1u;
}
