/*
 * policy.h — Radio fairness policy engine.
 *
 * Provides a lightweight, zero-allocation policy gate that enforces:
 *
 *   1. Class-based TTL caps:
 *        CONTROL  → TTL unlimited (max 7, no clamp)
 *        CHAT     → TTL capped at 5
 *        METRICS  → TTL capped at 3
 *        BULK     → TTL capped at 2
 *
 *   2. Airtime pre-check: reject relay if the per-node flood token bucket
 *      is exhausted (coarse flood-rate limiting per source node).
 *
 * The verdict is advisory; the caller decides how to act on it.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"   /* rivr_pkt_hdr_t, PKT_* constants */

/* ── Traffic classes ────────────────────────────────────────────────────── */
typedef enum {
    RIVR_CLASS_CONTROL = 0,  /* BEACON, ROUTE_REQ, ROUTE_RPL, ACK, PROG_PUSH */
    RIVR_CLASS_CHAT    = 1,  /* PKT_CHAT                                       */
    RIVR_CLASS_METRICS = 2,  /* PKT_METRICS / PKT_DATA with metrics marker     */
    RIVR_CLASS_BULK    = 3,  /* PKT_DATA (generic bulk payload)                */
} rivr_class_t;

/* TTL caps per class (0 = unlimited / use whatever is in the header) */
#define POLICY_TTL_CONTROL   0u   /* unlimited */
#define POLICY_TTL_CHAT      5u
#define POLICY_TTL_METRICS   3u
#define POLICY_TTL_BULK      2u

/* ── Policy verdict ─────────────────────────────────────────────────────── */
typedef enum {
    RIVR_POLICY_PASS      = 0,  /* forward as-is                              */
    RIVR_POLICY_TTL_CLAMP = 1,  /* forward but with TTL reduced to cap        */
    RIVR_POLICY_DROP      = 2,  /* discard this packet                        */
} rivr_policy_verdict_t;

/* ── Per-node flood token bucket ────────────────────────────────────────── */
/*
 * Each (src_node_id mod POLICY_BUCKET_COUNT) entry holds a token count that
 * refills at POLICY_REFILL_TOKENS_PER_SEC.  A relay attempt costs one token.
 *
 * With 16 buckets, 4 tokens, 1-token refill/s:
 *   – burst of 4 relays from the same source is fine
 *   – sustained rate is capped at 1 relay/s per bucket slot
 *   – hash collision between node IDs is benign (conservative)
 */
#define POLICY_BUCKET_COUNT        16u
#define POLICY_BUCKET_INIT_TOKENS   4u
#define POLICY_REFILL_TOKENS_PER_S  1u
#define POLICY_MAX_TOKENS           4u

typedef struct {
    uint8_t  tokens;       /* current token count                  */
    uint32_t last_refill;  /* timestamp of last refill (ms)        */
} policy_bucket_t;

typedef struct {
    policy_bucket_t buckets[POLICY_BUCKET_COUNT];
} policy_state_t;

/* ── API ────────────────────────────────────────────────────────────────── */

/** Initialise policy state — call once before the main loop. */
void policy_init(policy_state_t *ps);

/**
 * policy_check() — Evaluate the policy for a candidate relay frame.
 *
 * @param ps       Mutable policy state (token buckets + timestamps).
 * @param hdr      Decoded packet header of the frame to be relayed.
 * @param now_ms   Current monotonic time in milliseconds.
 * @param clamped_ttl_out  If verdict is RIVR_POLICY_TTL_CLAMP, the new TTL
 *                         is written here.  Unchanged for other verdicts.
 *
 * The function also performs the token-bucket deduction when the verdict is
 * PASS or TTL_CLAMP (i.e. the frame will be relayed).  When the verdict is
 * DROP the bucket is NOT debited, so the caller does not need to undo it.
 */
rivr_policy_verdict_t policy_check(policy_state_t         *ps,
                                   const rivr_pkt_hdr_t   *hdr,
                                   uint32_t                now_ms,
                                   uint8_t                *clamped_ttl_out);

/** Map a packet type byte to its traffic class. */
rivr_class_t policy_classify(uint8_t pkt_type);
