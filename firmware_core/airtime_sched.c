/**
 * @file  airtime_sched.c
 * @brief Airtime token-bucket scheduler implementation.
 *
 * See airtime_sched.h for full design rationale.
 */

#include "airtime_sched.h"
#include "protocol.h"       /* PKT_* constants, PKT_TYPE_BYTE_OFFSET, PKT_FLAG_RELAY */
#include "rivr_metrics.h"

#include <string.h>

/* ── Global context ──────────────────────────────────────────────────────── */

airtime_ctx_t g_airtime;

/* ── Init ─────────────────────────────────────────────────────────────────── */

void airtime_sched_init(void)
{
    memset(&g_airtime, 0, sizeof(g_airtime));
    g_airtime.tokens_us = AIRTIME_CAPACITY_US;
    /* nb[*].tokens_us and node_id left zero → allocated on first use */
}

/* ── Classification ──────────────────────────────────────────────────────── */

rivr_pkt_class_t rivr_pkt_classify(uint8_t pkt_type)
{
    switch (pkt_type) {
    case PKT_BEACON:     /* 2 – periodic presence advertisement           */
    case PKT_ROUTE_REQ:  /* 3 – route discovery                           */
    case PKT_ROUTE_RPL:  /* 4 – route reply                               */
    case PKT_ACK:        /* 5 – acknowledgement                           */
    case PKT_PROG_PUSH:  /* 7 – OTA program push                          */
        return PKTCLASS_CONTROL;

    case PKT_CHAT:       /* 1 – user message                              */
        return PKTCLASS_CHAT;

    case PKT_DATA:       /* 6 – sensor / metrics payload                  */
        return PKTCLASS_METRICS;

    default:             /* future or unrecognised                        */
        return PKTCLASS_BULK;
    }
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/** Refill the global bucket proportional to elapsed time. */
static void global_refill(uint32_t now_ms)
{
    if (now_ms == g_airtime.last_refill_ms) {
        return; /* sub-millisecond poll — nothing to add */
    }

    uint32_t elapsed_ms = now_ms - g_airtime.last_refill_ms;
    g_airtime.last_refill_ms = now_ms;

    /* Overflow-safe: AIRTIME_REFILL_US_PER_MS=100, elapsed fits uint32 */
    uint32_t add_us = elapsed_ms * AIRTIME_REFILL_US_PER_MS;
    uint32_t tokens = g_airtime.tokens_us + add_us;
    g_airtime.tokens_us = (tokens > AIRTIME_CAPACITY_US)
                          ? AIRTIME_CAPACITY_US : tokens;
}

/** Refill a per-neighbour bucket proportional to elapsed time. */
static void nb_refill(airtime_nb_bucket_t *nb, uint32_t now_ms)
{
    if (now_ms == nb->last_refill_ms) {
        return;
    }

    uint32_t elapsed_ms = now_ms - nb->last_refill_ms;
    nb->last_refill_ms = now_ms;

    uint32_t add_us = elapsed_ms * AIRTIME_NB_REFILL_US_PER_MS;
    uint32_t tokens = nb->tokens_us + add_us;
    nb->tokens_us = (tokens > AIRTIME_NB_CAPACITY_US)
                    ? AIRTIME_NB_CAPACITY_US : tokens;
}

/**
 * Find an existing per-neighbour slot for src_id, or allocate/evict one.
 * Never returns NULL.
 */
static airtime_nb_bucket_t *nb_get_or_alloc(uint32_t src_id, uint32_t now_ms)
{
    uint8_t  free_slot  = AIRTIME_NB_MAX;          /* first free (node_id==0) */
    uint8_t  lru_slot   = 0u;                      /* oldest last_refill_ms   */
    uint32_t lru_ts     = g_airtime.nb[0].last_refill_ms;

    for (uint8_t i = 0u; i < AIRTIME_NB_MAX; i++) {
        if (g_airtime.nb[i].node_id == src_id) {
            return &g_airtime.nb[i];     /* existing entry — fast path */
        }
        if (g_airtime.nb[i].node_id == 0u && free_slot == AIRTIME_NB_MAX) {
            free_slot = i;               /* first unused slot           */
        }
        if (g_airtime.nb[i].last_refill_ms < lru_ts) {
            lru_ts   = g_airtime.nb[i].last_refill_ms;
            lru_slot = i;
        }
    }

    /* Choose allocation target: prefer free slot, else evict LRU */
    uint8_t slot = (free_slot < AIRTIME_NB_MAX) ? free_slot : lru_slot;

    g_airtime.nb[slot].node_id        = src_id;
    g_airtime.nb[slot].tokens_us      = AIRTIME_NB_CAPACITY_US;
    g_airtime.nb[slot].last_refill_ms = now_ms;
    return &g_airtime.nb[slot];
}

/** Increment the per-class drop counter. */
static void record_class_drop(rivr_pkt_class_t cls)
{
    switch (cls) {
    case PKTCLASS_CONTROL: g_rivr_metrics.class_drops_ctrl++;    break;
    case PKTCLASS_CHAT:    g_rivr_metrics.class_drops_chat++;    break;
    case PKTCLASS_METRICS: g_rivr_metrics.class_drops_metrics++; break;
    case PKTCLASS_BULK:    g_rivr_metrics.class_drops_bulk++;    break;
    default: break;
    }
}

/* ── Public gate ─────────────────────────────────────────────────────────── */

bool airtime_sched_check_consume(const uint8_t *frame_data,
                                 uint8_t        frame_len,
                                 uint32_t       toa_us,
                                 uint32_t       now_ms)
{
    /* ── Step 1: classify ──────────────────────────────────────────────── */
    uint8_t pkt_type = 0u;
    if (frame_data != NULL && (uint32_t)frame_len > PKT_TYPE_BYTE_OFFSET) {
        pkt_type = frame_data[PKT_TYPE_BYTE_OFFSET];
    }
    rivr_pkt_class_t cls = rivr_pkt_classify(pkt_type);

    /* ── Step 2: CONTROL always passes, no consumption ────────────────── */
    if (cls == PKTCLASS_CONTROL) {
        return true;
    }

    /* ── Step 3: refill global bucket ─────────────────────────────────── */
    global_refill(now_ms);

    /* ── Step 4: check global budget ──────────────────────────────────── */
    if (g_airtime.tokens_us < toa_us) {
        record_class_drop(cls);
        return false;
    }

    /* ── Step 5: consume global tokens ───────────────────────────────── */
    g_airtime.tokens_us -= toa_us;

    /* ── Step 6: low-watermark check ─────────────────────────────────── */
    if (g_airtime.tokens_us < AIRTIME_LOW_WATERMARK_US) {
        g_rivr_metrics.airtime_tokens_low++;
    }

    /* ── Step 7: per-source relay fairness check ─────────────────────── *
     * Only for frames that have been relayed (PKT_FLAG_RELAY set).       *
     * src_id lives at wire bytes [9..12] (little-endian uint32).         *
     * Header layout from protocol.h:                                     *
     *   offset 4  → flags                                                *
     *   offset 9  → src_id byte 0 (LSB)                                  *
     *   offset 12 → src_id byte 3 (MSB)                                  */
    if (frame_data != NULL && frame_len > 12u) {
        uint8_t flags = frame_data[4u];
        if (flags & PKT_FLAG_RELAY) {
            uint32_t src_id = (uint32_t)frame_data[9u]
                            | ((uint32_t)frame_data[10u] <<  8u)
                            | ((uint32_t)frame_data[11u] << 16u)
                            | ((uint32_t)frame_data[12u] << 24u);

            if (src_id != 0u) {
                airtime_nb_bucket_t *nb = nb_get_or_alloc(src_id, now_ms);
                nb_refill(nb, now_ms);

                if (nb->tokens_us < toa_us) {
                    /* Source over its relay budget: undo global consume */
                    uint32_t restored = g_airtime.tokens_us + toa_us;
                    g_airtime.tokens_us = (restored > AIRTIME_CAPACITY_US)
                                         ? AIRTIME_CAPACITY_US : restored;
                    record_class_drop(cls);
                    return false;
                }
                nb->tokens_us -= toa_us;
            }
        }
    }

    return true;
}
