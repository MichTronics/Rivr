/*
 * policy.c — Radio fairness policy engine implementation.
 *
 * See policy.h for the full description of the algorithm.
 */

#include "policy.h"
#include "protocol.h"  /* PKT_* constants */
#include <string.h>

/* ── policy_init ─────────────────────────────────────────────────────────── */

void policy_init(policy_state_t *ps)
{
    for (unsigned i = 0; i < POLICY_BUCKET_COUNT; i++) {
        ps->buckets[i].tokens      = POLICY_BUCKET_INIT_TOKENS;
        ps->buckets[i].last_refill = 0u;
    }
}

/* ── policy_classify ─────────────────────────────────────────────────────── */

rivr_class_t policy_classify(uint8_t pkt_type)
{
    switch (pkt_type) {
        case PKT_CHAT:      return RIVR_CLASS_CHAT;
        case PKT_DATA:      return RIVR_CLASS_BULK;
        /* Control-plane frames */
        case PKT_BEACON:    /* fall-through */
        case PKT_ROUTE_REQ: /* fall-through */
        case PKT_ROUTE_RPL: /* fall-through */
        case PKT_ACK:       /* fall-through */
        case PKT_PROG_PUSH: /* fall-through */
        default:            return RIVR_CLASS_CONTROL;
    }
}

/* ── policy_check ────────────────────────────────────────────────────────── */

rivr_policy_verdict_t policy_check(policy_state_t       *ps,
                                   const rivr_pkt_hdr_t *hdr,
                                   uint32_t              now_ms,
                                   uint8_t              *clamped_ttl_out)
{
    rivr_class_t cls = policy_classify(hdr->pkt_type);

    /* ── 1. TTL cap ── */
    uint8_t cap = 0u;
    switch (cls) {
        case RIVR_CLASS_CHAT:    cap = POLICY_TTL_CHAT;    break;
        case RIVR_CLASS_METRICS: cap = POLICY_TTL_METRICS; break;
        case RIVR_CLASS_BULK:    cap = POLICY_TTL_BULK;    break;
        default:                 cap = 0u; break;  /* CONTROL: no cap */
    }

    rivr_policy_verdict_t verdict = RIVR_POLICY_PASS;
    if (cap > 0u && hdr->ttl > cap) {
        *clamped_ttl_out = cap;
        verdict = RIVR_POLICY_TTL_CLAMP;
    }

    /* ── 2. Token-bucket flood gate (CONTROL always bypasses) ── */
    if (cls != RIVR_CLASS_CONTROL) {
        uint32_t idx = (uint32_t)(hdr->src_id % POLICY_BUCKET_COUNT);
        policy_bucket_t *b = &ps->buckets[idx];

        /* Refill tokens proportional to elapsed seconds */
        if (now_ms >= b->last_refill) {
            uint32_t elapsed_s = (now_ms - b->last_refill) / 1000u;
            if (elapsed_s > 0u) {
                uint32_t add      = elapsed_s * (uint32_t)POLICY_REFILL_TOKENS_PER_S;
                uint32_t new_tok  = (uint32_t)b->tokens + add;
                b->tokens         = (uint8_t)((new_tok > POLICY_MAX_TOKENS)
                                               ? POLICY_MAX_TOKENS : new_tok);
                b->last_refill   += elapsed_s * 1000u;
            }
        } else {
            /* Clock wrapped or was reset — reset the bucket timestamp */
            b->last_refill = now_ms;
        }

        if (b->tokens == 0u) {
            return RIVR_POLICY_DROP;
        }
        b->tokens--;
    }

    return verdict;
}
