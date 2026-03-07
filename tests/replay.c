/**
 * @file  replay.c
 * @brief Deterministic replay / sim harness — implementation.
 *
 * See replay.h for the JSONL trace format and design rationale.
 *
 * Compile together with: protocol.c routing.c route_cache.c
 *   rivr_metrics.c rivr_log.c airtime_sched.c test_stubs.c
 * Compiler flags: -std=c11 -I.. -I../firmware_core
 */

/* Must come before any firmware header on the host build */
#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

#include "replay.h"
#include "protocol.h"
#include "routing.h"
#include "route_cache.h"
#include "rivr_metrics.h"
#include "airtime_sched.h"
#include "radio_sx1262.h"   /* rf_tx_request_t, rf_tx_queue, RF_MAX_PAYLOAD_LEN */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

/* ── Clock stub (defined in test_stubs.c) ─────────────────────────────────── */
extern void test_set_ms(uint32_t abs_ms);

/* ── Assertion helpers ────────────────────────────────────────────────────── */

#define REPLAY_OK(ctx, msg) do {                                         \
    printf("  OK   [replay] %s\n", (msg));                              \
    (ctx)->pass++;                                                       \
} while (0)

#define REPLAY_FAIL(ctx, msg) do {                                       \
    printf("FAIL  [replay] %s\n", (msg));                               \
    (ctx)->fail++;                                                       \
} while (0)

/* ── Minimal JSON field extraction ──────────────────────────────────────────── *
 *
 * These helpers search a single JSON line for a specific key and extract
 * its value.  They handle our fixed schema only — no recursive parsing.
 * ─────────────────────────────────────────────────────────────────────────── */

/**
 * Extract a string value: "key":"value".
 * Returns 1 on success (out is null-terminated), 0 if key not found.
 */
static int json_str(const char *ln, const char *key,
                     char *out, size_t cap)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(ln, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1u < cap) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/**
 * Extract an unsigned decimal integer value: "key":N.
 * Returns 1 on success, 0 if key not found or parse failed.
 */
static int json_u32(const char *ln, const char *key, uint32_t *out)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(ln, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    unsigned long v;
    if (sscanf(p, "%lu", &v) != 1) return 0;
    *out = (uint32_t)v;
    return 1;
}

/**
 * Extract a signed decimal integer value: "key":-N or "key":N.
 * Returns 1 on success, 0 if key not found or parse failed.
 */
static int json_i32(const char *ln, const char *key, int32_t *out)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(ln, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    long v;
    if (sscanf(p, "%ld", &v) != 1) return 0;
    *out = (int32_t)v;
    return 1;
}

/**
 * Extract a hex node ID printed as a quoted string: "src":"0xAAAA0001".
 * Returns 1 on success, 0 if key not found or not a valid hex string.
 */
static int json_hex(const char *ln, const char *key, uint32_t *out)
{
    char buf[16] = "";
    if (!json_str(ln, key, buf, sizeof(buf))) return 0;
    unsigned long v;
    if (sscanf(buf, "0x%lx", &v) != 1 && sscanf(buf, "0X%lx", &v) != 1) {
        /* Try plain decimal as fallback */
        if (sscanf(buf, "%lu", &v) != 1) return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* ── Metric lookup table ─────────────────────────────────────────────────── */

typedef struct { const char *name; size_t offset; } metric_entry_t;

static const metric_entry_t s_metric_map[] = {
    /* reception */
    { "rx_decode_fail",    offsetof(rivr_metrics_t, rx_decode_fail)    },
    { "rx_dedupe_drop",    offsetof(rivr_metrics_t, rx_dedupe_drop)    },
    { "rx_ttl_drop",       offsetof(rivr_metrics_t, rx_ttl_drop)       },
    /* TX queue */
    { "tx_queue_full",     offsetof(rivr_metrics_t, tx_queue_full)     },
    { "tx_queue_peak",     offsetof(rivr_metrics_t, tx_queue_peak)     },
    /* duty cycle */
    { "duty_blocked",      offsetof(rivr_metrics_t, duty_blocked)      },
    /* fabric */
    { "fabric_drop",       offsetof(rivr_metrics_t, fabric_drop)       },
    { "fabric_delay",      offsetof(rivr_metrics_t, fabric_delay)      },
    /* radio */
    { "radio_busy_stall",  offsetof(rivr_metrics_t, radio_busy_stall)  },
    { "radio_tx_fail",     offsetof(rivr_metrics_t, radio_tx_fail)     },
    { "radio_hard_reset",  offsetof(rivr_metrics_t, radio_hard_reset)  },
    { "radio_rx_crc_fail", offsetof(rivr_metrics_t, radio_rx_crc_fail) },
    { "radio_rx_timeout",  offsetof(rivr_metrics_t, radio_rx_timeout)  },
    { "radio_reset_backoff",offsetof(rivr_metrics_t,radio_reset_backoff)},
    /* pending queue */
    { "pq_dropped",        offsetof(rivr_metrics_t, pq_dropped)        },
    { "pq_expired",        offsetof(rivr_metrics_t, pq_expired)        },
    { "pq_peak",           offsetof(rivr_metrics_t, pq_peak)           },
    /* routing */
    { "rcache_evict",      offsetof(rivr_metrics_t, rcache_evict)      },
    { "drop_no_route",     offsetof(rivr_metrics_t, drop_no_route)     },
    { "drop_rate_limited", offsetof(rivr_metrics_t, drop_rate_limited) },
    { "drop_ttl_relay",    offsetof(rivr_metrics_t, drop_ttl_relay)    },
    /* misc */
    { "loop_jitter_ms",    offsetof(rivr_metrics_t, loop_jitter_ms)    },
    /* step 6: airtime */
    { "airtime_tokens_low",offsetof(rivr_metrics_t, airtime_tokens_low)},
    { "class_drops_ctrl",  offsetof(rivr_metrics_t, class_drops_ctrl)  },
    { "class_drops_chat",  offsetof(rivr_metrics_t, class_drops_chat)  },
    { "class_drops_metrics",offsetof(rivr_metrics_t,class_drops_metrics)},
    { "class_drops_bulk",  offsetof(rivr_metrics_t, class_drops_bulk)  },
};

static const uint32_t *metric_lookup(const char *name)
{
    size_t n = sizeof(s_metric_map) / sizeof(s_metric_map[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s_metric_map[i].name, name) == 0) {
            return (const uint32_t *)((const char *)&g_rivr_metrics
                                      + s_metric_map[i].offset);
        }
    }
    return NULL;
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void handle_comment(const char *line)
{
    char msg[256] = "";
    json_str(line, "msg", msg, sizeof(msg));
    printf("  -- %s\n", msg);
}

static void handle_tick(replay_ctx_t *ctx, const char *line)
{
    uint32_t t = 0;
    json_u32(line, "t_ms", &t);
    if (t > ctx->now_ms) ctx->now_ms = t;
    test_set_ms(ctx->now_ms);
}

static void handle_fault(replay_ctx_t *ctx, const char *line)
{
    char name[32] = "";
    json_str(line, "name", name, sizeof(name));

    if (strcmp(name, "crc_fail") == 0) {
        uint32_t count = 0;
        json_u32(line, "count", &count);
        ctx->fault.crc_fail_remaining = count;
        return;
    }
    if (strcmp(name, "busy_stuck") == 0) {
        uint32_t count = 0;
        json_u32(line, "count", &count);
        ctx->fault.busy_stuck_remaining = count;
        return;
    }
    if (strcmp(name, "tx_timeout") == 0) {
        uint32_t count = 0;
        json_u32(line, "count", &count);
        ctx->fault.tx_timeout_remaining = count;
        return;
    }
    if (strcmp(name, "queue_fill") == 0) {
        /* Pre-fill rf_tx_queue with dummy frames */
        uint32_t count = 0;
        json_u32(line, "count", &count);
        rf_tx_request_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        dummy.len = 1u;
        dummy.data[0] = 0xDEu;
        dummy.toa_us = 1u;
        for (uint32_t i = 0; i < count; i++) {
            if (!rb_try_push(&rf_tx_queue, &dummy)) break;
        }
        return;
    }
    if (strcmp(name, "drain_tokens") == 0) {
        /* Deplete the global airtime token bucket immediately */
        g_airtime.tokens_us      = 0u;
        g_airtime.last_refill_ms = ctx->now_ms;
        /* Also per-neighbour buckets (zero them all) */
        for (uint8_t i = 0; i < AIRTIME_NB_MAX; i++) {
            g_airtime.nb[i].tokens_us      = 0u;
            g_airtime.nb[i].last_refill_ms = ctx->now_ms;
        }
        return;
    }
    fprintf(stderr, "WARN: unknown fault name '%s'\n", name);
}

static void handle_rx_frame(replay_ctx_t *ctx, const char *line)
{
    /* ── Parse fields ──────────────────────────────────────────────────── */
    uint32_t t_ms     = ctx->now_ms;
    uint32_t src      = 0u;
    uint32_t dst      = 0u;
    uint32_t seq      = 0u;
    uint32_t type_u   = (uint32_t)PKT_CHAT;
    uint32_t ttl_u    = 7u;
    uint32_t hop_u    = 0u;
    int32_t  rssi     = -80;
    int32_t  snr      = 5;
    uint32_t fwd_toa  = REPLAY_DEFAULT_TOA_US;
    char     payload[RIVR_PKT_MAX_PAYLOAD + 1u] = "";

    json_u32(line, "t_ms",      &t_ms);
    json_hex(line, "src",       &src);
    json_hex(line, "dst",       &dst);
    json_u32(line, "seq",       &seq);
    json_u32(line, "type",      &type_u);
    json_u32(line, "ttl",       &ttl_u);
    json_u32(line, "hop",       &hop_u);
    json_i32(line, "rssi",      &rssi);
    json_i32(line, "snr",       &snr);
    json_u32(line, "fwd_toa_us",&fwd_toa);
    json_str(line, "payload",   payload, sizeof(payload));

    /* Advance simulated clock if this frame's timestamp is newer */
    if (t_ms > ctx->now_ms) {
        ctx->now_ms = t_ms;
        test_set_ms(ctx->now_ms);
    }

    /* ── Encode as a real wire frame ───────────────────────────────────── */
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = RIVR_MAGIC;
    hdr.version     = RIVR_PROTO_VER;
    hdr.pkt_type    = (uint8_t)type_u;
    hdr.flags       = 0u;
    hdr.ttl         = (uint8_t)ttl_u;
    hdr.hop         = (uint8_t)hop_u;
    hdr.net_id      = 0u;
    hdr.src_id      = src;
    hdr.dst_id      = dst;
    hdr.seq         = seq;
    hdr.payload_len = (uint8_t)strlen(payload);

    uint8_t wire[RIVR_PKT_HDR_LEN + RIVR_PKT_MAX_PAYLOAD + RIVR_PKT_CRC_LEN];
    int wlen = protocol_encode(&hdr,
                               (const uint8_t *)payload,
                               hdr.payload_len,
                               wire, sizeof(wire));
    if (wlen < 0) {
        fprintf(stderr, "WARN: protocol_encode failed (frame too large?)\n");
        return;
    }

    /* ── CRC-fail fault injection ──────────────────────────────────────── */
    if (ctx->fault.crc_fail_remaining > 0u) {
        ctx->fault.crc_fail_remaining--;
        wire[wlen - 1u] ^= 0xFFu;   /* corrupt last CRC byte */
        g_rivr_metrics.rx_decode_fail++;
        g_rivr_metrics.radio_rx_crc_fail++;
        return;  /* frame rejected; no further processing */
    }

    /* ── Decode and validate ───────────────────────────────────────────── */
    rivr_pkt_hdr_t    dec_hdr;
    const uint8_t    *dec_payload = NULL;
    if (!protocol_decode(wire, (uint8_t)wlen, &dec_hdr, &dec_payload)) {
        g_rivr_metrics.rx_decode_fail++;
        return;
    }

    /* ── Neighbour update (before flood decision) ─────────────────────── */
    routing_neighbor_update(&ctx->nb, &dec_hdr,
                             (int8_t)rssi, (int8_t)snr,
                             ctx->now_ms);

    /* ── Reverse-path route learning ──────────────────────────────────── */
    /*  from_id == src_id when hop == 0 (direct link).                    */
    route_cache_learn_rx(&ctx->rc,
                          dec_hdr.src_id,
                          dec_hdr.src_id,   /* from_id: direct in replay  */
                          dec_hdr.hop,
                          (int16_t)rssi, (int8_t)snr,
                          ctx->now_ms);

    /* ── Flood-forward decision (dedupe + TTL + loop + budget) ──────────────── */
    rivr_fwd_result_t fwd = routing_flood_forward(&ctx->dc,
                                                   &ctx->fb,
                                                   &dec_hdr,
                                                   ctx->my_id,
                                                   fwd_toa,
                                                   ctx->now_ms);
    switch (fwd) {
    case RIVR_FWD_DROP_DEDUPE:
        /* rx_dedupe_drop already incremented inside routing.c */
        return;
    case RIVR_FWD_DROP_TTL:
        g_rivr_metrics.drop_ttl_relay++;
        return;
    case RIVR_FWD_DROP_BUDGET:
        g_rivr_metrics.drop_rate_limited++;
        return;
    case RIVR_FWD_DROP_LOOP:
        /* loop_detect_drop already incremented inside routing.c */
        return;
    case RIVR_FWD_FORWARD:
        break;  /* proceed to TX path */
    }

    /* ── Re-encode mutated header (TTL--, hop++, PKT_FLAG_RELAY set) ──── */
    uint8_t fwd_wire[RIVR_PKT_HDR_LEN + RIVR_PKT_MAX_PAYLOAD + RIVR_PKT_CRC_LEN];
    int fwd_len = protocol_encode(&dec_hdr,
                                   dec_payload,
                                   dec_hdr.payload_len,
                                   fwd_wire, sizeof(fwd_wire));
    if (fwd_len < 0) return;

    /* ── Airtime gate (checked on the relay-flagged frame) ─────────────── */
    if (!airtime_sched_check_consume(fwd_wire, (uint8_t)fwd_len,
                                      fwd_toa, ctx->now_ms)) {
        return;  /* class_drops_* already incremented inside airtime_sched */
    }

    /* ── TX fault injection ────────────────────────────────────────────── */
    if (ctx->fault.busy_stuck_remaining > 0u) {
        ctx->fault.busy_stuck_remaining--;
        g_rivr_metrics.radio_busy_stall++;
        return;
    }
    if (ctx->fault.tx_timeout_remaining > 0u) {
        ctx->fault.tx_timeout_remaining--;
        g_rivr_metrics.radio_tx_fail++;
        return;
    }

    /* ── Enqueue for TX ───────────────────────────────────────────────── */
    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    uint8_t n = ((uint8_t)fwd_len <= RF_MAX_PAYLOAD_LEN)
                ? (uint8_t)fwd_len
                : RF_MAX_PAYLOAD_LEN;
    memcpy(req.data, fwd_wire, n);
    req.len    = n;
    req.toa_us = fwd_toa;
    req.due_ms = 0u;

    if (!rb_try_push(&rf_tx_queue, &req)) {
        g_rivr_metrics.tx_queue_full++;
    }
}

static void handle_assert(replay_ctx_t *ctx, const char *line)
{
    char what[32] = "";
    char op[8]    = "";
    char msg[REPLAY_MAX_LINE] = "";
    json_str(line, "what", what, sizeof(what));
    json_str(line, "op",   op,   sizeof(op));

    /* ── metric assertion ──────────────────────────────────────────────── */
    if (strcmp(what, "metric") == 0) {
        char   key[48] = "";
        uint32_t val   = 0;
        json_str(line, "key", key, sizeof(key));
        json_u32(line, "val", &val);

        const uint32_t *field = metric_lookup(key);
        if (!field) {
            snprintf(msg, sizeof(msg), "metric '%s' not found in map", key);
            REPLAY_FAIL(ctx, msg);
            return;
        }
        uint32_t actual = *field;
        bool ok = false;

        if      (strcmp(op, "eq")  == 0) ok = (actual == val);
        else if (strcmp(op, "ne")  == 0) ok = (actual != val);
        else if (strcmp(op, "gt")  == 0) ok = (actual >  val);
        else if (strcmp(op, "gte") == 0) ok = (actual >= val);
        else if (strcmp(op, "lt")  == 0) ok = (actual <  val);
        else {
            snprintf(msg, sizeof(msg), "unknown op '%s'", op);
            REPLAY_FAIL(ctx, msg);
            return;
        }

        snprintf(msg, sizeof(msg),
                 "metric %s %s %" PRIu32 " (actual=%" PRIu32 ")",
                 key, op, val, actual);
        if (ok) REPLAY_OK(ctx, msg);
        else    REPLAY_FAIL(ctx, msg);
        return;
    }

    /* ── neighbor_count assertion ──────────────────────────────────────── */
    if (strcmp(what, "neighbor_count") == 0) {
        uint32_t val    = 0;
        json_u32(line, "val", &val);
        uint32_t actual = routing_neighbor_count(&ctx->nb, ctx->now_ms);
        bool ok = false;

        if      (strcmp(op, "eq")  == 0) ok = (actual == val);
        else if (strcmp(op, "gte") == 0) ok = (actual >= val);
        else if (strcmp(op, "lt")  == 0) ok = (actual <  val);

        snprintf(msg, sizeof(msg),
                 "neighbor_count %s %" PRIu32 " (actual=%" PRIu32 ")",
                 op, val, actual);
        if (ok) REPLAY_OK(ctx, msg);
        else    REPLAY_FAIL(ctx, msg);
        return;
    }

    /* ── route assertion ───────────────────────────────────────────────── */
    if (strcmp(what, "route") == 0) {
        uint32_t node = 0u;
        json_hex(line, "node", &node);
        const route_cache_entry_t *e = route_cache_lookup(&ctx->rc,
                                                           node,
                                                           ctx->now_ms);
        bool exists = (e != NULL && (e->flags & RCACHE_FLAG_VALID));
        bool want   = (strcmp(op, "exists") == 0);

        snprintf(msg, sizeof(msg),
                 "route to 0x%08" PRIx32 " %s",
                 node, op);
        if (exists == want) REPLAY_OK(ctx, msg);
        else                REPLAY_FAIL(ctx, msg);
        return;
    }

    snprintf(msg, sizeof(msg), "unknown assert what='%s'", what);
    REPLAY_FAIL(ctx, msg);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void replay_ctx_init(replay_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->now_ms = 0u;

    routing_dedupe_init(&ctx->dc);
    routing_neighbor_init(&ctx->nb);
    route_cache_init(&ctx->rc);
    routing_fwdbudget_init(&ctx->fb);

    memset(&ctx->fault, 0, sizeof(ctx->fault));

    /* Reset global singletons that the harness exercises */
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));
    airtime_sched_init();

    test_set_ms(0u);
}

int replay_run_file(replay_ctx_t *ctx, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "replay: cannot open '%s'\n", path);
        ctx->fail++;
        return 1;
    }

    /* Reset per-file counters */
    ctx->pass = 0u;
    ctx->fail = 0u;

    printf("\n── Trace: %s ─────────────────────────────────────────────────\n",
           path);

    char line[REPLAY_MAX_LINE];
    while (fgets(line, (int)sizeof(line), fp)) {
        /* Strip trailing newline and carriage return */
        size_t len = strlen(line);
        while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r'))
            line[--len] = '\0';

        /* Skip blank lines and # comments */
        if (len == 0u || line[0] == '#') continue;

        /* Extract "ev" field */
        char ev[32] = "";
        if (!json_str(line, "ev", ev, sizeof(ev))) continue;

        if      (strcmp(ev, "comment")  == 0) handle_comment(line);
        else if (strcmp(ev, "tick")     == 0) handle_tick(ctx, line);
        else if (strcmp(ev, "fault")    == 0) handle_fault(ctx, line);
        else if (strcmp(ev, "rx_frame") == 0) handle_rx_frame(ctx, line);
        else if (strcmp(ev, "assert")   == 0) handle_assert(ctx, line);
        else fprintf(stderr, "WARN: unknown replay event type '%s'\n", ev);
    }

    fclose(fp);

    printf("── result: %s — pass=%" PRIu32 " fail=%" PRIu32 "\n",
           ctx->fail == 0u ? "PASS" : "FAIL",
           ctx->pass, ctx->fail);
    return (int)ctx->fail;
}
