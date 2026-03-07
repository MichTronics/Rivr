/**
 * @file  fuzz_ffi_harness.c
 * @brief AFL++/libFuzzer-compatible fuzz harness for the RIVR C packet layer.
 *
 * Targets:
 *  1. protocol_decode()     — parse arbitrary bytes as a RIVR wire frame.
 *  2. routing_flood_forward() — exercise the routing/dedupe logic on any
 *                               successfully decoded packet.
 *  3. routing_jitter_ticks()  — deterministic jitter helper (no crash path).
 *
 * Invariants asserted on every input:
 *  - protocol_decode() never crashes; returns bool (no UB).
 *  - routing_flood_forward() returns one of the four defined enum values.
 *  - routing_jitter_ticks() returns a value in [0..max_j].
 *
 * Build with AFL++ persistent mode:
 *   AFL_USE_ASAN=1 afl-cc -std=c11 -g -O1 \
 *       -I../firmware_core -I../  -Iinclude \
 *       ../firmware_core/protocol.c \
 *       ../firmware_core/routing.c  \
 *       ../firmware_core/route_cache.c \
 *       ../firmware_core/rivr_metrics.c \
 *       ../firmware_core/rivr_log.c     \
 *       ../firmware_core/airtime_sched.c \
 *       test_stubs.c fuzz_ffi_harness.c \
 *       -o fuzz_ffi_harness
 *
 * Run:
 *   mkdir -p fuzz_corpus fuzz_findings
 *   printf '\x52\x56\x01' > fuzz_corpus/seed0   # "RV\x01" magic + version
 *   afl-fuzz -i fuzz_corpus -o fuzz_findings -- ./fuzz_ffi_harness
 *
 * Or with plain stdin (no AFL++):
 *   echo 'RVXX' | ./fuzz_ffi_harness
 */

#define _GNU_SOURCE   /* getchar_unlocked */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "protocol.h"
#include "routing.h"

/* ── AFL++ persistent-mode shim ─────────────────────────────────────────── */
/*
 * When compiled with afl-cc the macros below are real; when compiled with a
 * plain C compiler they expand to single-iteration stubs so the binary can
 * be used as a standalone reproducer.
 */
#ifndef __AFL_HAVE_MANUAL_CONTROL
#  define __AFL_HAVE_MANUAL_CONTROL 0
#  define __AFL_INIT()               ((void)0)
#  define __AFL_LOOP(n)              ((__afl_loop_counter++ == 0) ? 1 : 0)
static unsigned int __afl_loop_counter = 0;
#endif

/* ── Read up to cap bytes from stdin, return actual length. ─────────────── */
static size_t read_stdin(uint8_t *buf, size_t cap)
{
    size_t total = 0;
    int c;
    while (total < cap) {
        c = getchar_unlocked();
        if (c == EOF) break;
        buf[total++] = (uint8_t)c;
    }
    return total;
}

/* ── Main fuzz loop ───────────────────────────────────────────────────────── */
int main(void)
{
    /* Static module state (zero-initialised; survives across AFL iterations). */
    static dedupe_cache_t   cache;
    static forward_budget_t budget;
    static bool             inited = false;

    if (!inited) {
        routing_dedupe_init(&cache);
        routing_fwdbudget_init(&budget);
        inited = true;
    }

    __AFL_INIT();

    /*
     * Maximum frame: RIVR_PKT_HDR_LEN (22) + RIVR_PKT_MAX_PAYLOAD + 2-byte CRC.
     * 255 bytes is the absolute upper bound on any on-air RIVR frame.
     */
    uint8_t buf[255];

    while (__AFL_LOOP(1000)) {

        size_t len = read_stdin(buf, sizeof(buf));
        if (len == 0) continue;

        /* ── 1. Fuzz protocol_decode ───────────────────────────────────── */
        rivr_pkt_hdr_t   hdr;
        const uint8_t   *payload = NULL;

        memset(&hdr, 0, sizeof(hdr));

        /*
         * Must not crash or invoke UB on any input.
         * Truncate len to uint8_t since protocol_decode takes uint8_t len.
         */
        uint8_t frame_len = (len > 255u) ? 255u : (uint8_t)len;
        bool ok = protocol_decode(buf, frame_len, &hdr, &payload);

        /* ── 2. Fuzz routing layer on successfully decoded packets ─────── */
        if (ok) {
            /* payload_out must point inside buf when decode succeeds. */
            if (hdr.payload_len > 0) {
                assert(payload != NULL);
                assert(payload >= buf && payload < buf + frame_len);
            }

            rivr_fwd_result_t result =
                routing_flood_forward(&cache, &budget, &hdr,
                                      /*my_id=*/0u,
                                      /*toa_us=*/1000u,
                                      /*now_ms=*/0u);

            /* Verify result is one of the five defined enum values. */
            assert(result == RIVR_FWD_FORWARD    ||
                   result == RIVR_FWD_DROP_DEDUPE ||
                   result == RIVR_FWD_DROP_TTL    ||
                   result == RIVR_FWD_DROP_BUDGET  ||
                   result == RIVR_FWD_DROP_LOOP);
        }

        /* ── 3. Fuzz jitter helper (always safe to call) ──────────────── */
        uint32_t src_seed = 0, seq_seed = 0;
        if (len >= 4) memcpy(&src_seed, buf,     4);
        if (len >= 8) memcpy(&seq_seed, buf + 4, 4);

        uint16_t max_j  = (len >= 10) ? (uint16_t)((buf[8] << 8) | buf[9]) : 200u;
        if (max_j == 0) max_j = 1;   /* avoid degenerate case */

        uint16_t jitter = routing_jitter_ticks(src_seed, (uint16_t)seq_seed, max_j);
        assert(jitter <= max_j);
    }

    return 0;
}
