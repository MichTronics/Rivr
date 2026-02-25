/**
 * @file  main.c
 * @brief ESP32 firmware entry point — RIVR-driven LoRa node.
 *
 * MAIN LOOP STRUCTURE
 * ═══════════════════
 *
 *  app_main()
 *   │
 *   ├─ platform_init()        hardware: GPIO, SPI, LED
 *   ├─ timebase_init()        start 1-ms monotonic timer
 *   ├─ radio_init()           configure SX1262, attach ISR to DIO1
 *   ├─ dutycycle_init()       zero out DC sliding-window context
 *   ├─ rivr_embed_init()      parse RIVR program, register sinks
 *   └─ radio_start_rx()       start continuous receive
 *        │
 *        └─ loop forever:
 *              rivr_tick()          ← drain RX, run engine, emit to sinks
 *              tx_drain_loop()      ← send queued TX frames (with DC gate)
 *              platform_led_toggle()← heartbeat
 *              vTaskDelay(1ms)
 *
 * ISR PATH (DIO1 fires on RxDone):
 *   radio_isr() ──► rf_rx_ringbuf.try_push()    (no RIVR call, no alloc)
 *
 * EMIT PATH:
 *   RIVR emit rf_tx(chat) ──► rf_tx_sink_cb() ──► rf_tx_queue.try_push()
 *   tx_drain_loop()           ──► dutycycle_check() ──► radio_transmit()
 *
 * DETERMINISM + BOUNDED MEMORY GUARANTEES
 * ──────────────────────────────────────────
 *  • No dynamic memory allocation after app_main() initialisation.
 *  • rivr_tick() is bounded: drains at most SOURCES_RF_RX_DRAIN_LIMIT frames
 *    and runs the engine for at most MAX_ENGINE_STEPS scheduler cycles.
 *  • TX is bounded: tx_drain_loop() sends at most TX_DRAIN_LIMIT frames per
 *    main-loop iteration, each gated by the duty-cycle limiter.
 *  • Ring-buffer drops are counted and can be read via `rivr_embed_print_stats()`.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "firmware_core/platform_esp32.h"
#include "firmware_core/timebase.h"
#include "firmware_core/radio_sx1262.h"
#include "firmware_core/dutycycle.h"
#include "firmware_core/protocol.h"
#include "firmware_core/routing.h"
#include "firmware_core/route_cache.h"
#include "firmware_core/pending_queue.h"
#include "rivr_layer/rivr_embed.h"
#include "rivr_layer/rivr_sinks.h"
#include "firmware_core/display/display.h"

#define TAG              "MAIN"
#define TX_DRAIN_LIMIT   2u     /**< Max TX frames sent per main-loop iteration */
#define STATS_INTERVAL_MS 30000u /**< Print stats every 30 s                   */

/** Compile-time node callsign — override with -DRIVR_CALLSIGN="N0CALL" */
#ifndef RIVR_CALLSIGN
#  define RIVR_CALLSIGN "RIVR"
#endif
/** Compile-time network discriminator — override with -DRIVR_NET_ID=0x1234 */
#ifndef RIVR_NET_ID
#  define RIVR_NET_ID 0u
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * SIMULATION MODE  (compiled in when -DRIVR_SIM_MODE=1)
 *
 * PHASE A+D demonstration — 3 virtual mesh nodes:
 *
 *   MY_NODE  (0xFEED0000)  ← this firmware instance
 *   NODE_A   (0xAAAA0001)  ← original chat sender, direct neighbour
 *   NODE_B   (0xBBBB0002)  ← relay node, also direct neighbour
 *   NODE_C   (0xCCCC0003)  ← 2-hop remote node reached via NODE_B
 *
 * Injection sequence:
 *   Round 1: 3 × PKT_CHAT from NODE_A  hop=0  from_id=A
 *            → dedupe inserts, RIVR passes, relay queued, route_cache[A]=direct
 *            GATE1: forwarded frames must show ttl-1 / hop+1
 *   Round 2: 2 × PKT_CHAT from NODE_C  hop=1  from_id=B
 *            → dedupe inserts, RIVR passes, relay queued, route_cache[C]=via B
 *            GATE3: route_cache[C].next_hop == B  (reverse-path learning)
 *   Round 3: 1 × PKT_DATA from NODE_A  (type=6, RIVR drops, C relay queued)
 *            GATE1: forwarded frame still checked for ttl-1/hop+1
 *   Round 4: 1 × PKT_ROUTE_REQ from NODE_B asking for MY_NODE
 *            → firmware detects it is the target → builds + enqueues ROUTE_RPL
 *   Round 5: Exact duplicate of R1[0] via same relay
 *            → DEDUPE-DROP (GATE2 basic case)
 *   Round 6: Same (src_id=NODE_A, seq=0) re-forwarded by NODE_B relay
 *            → DEDUPE-DROP despite different from_id (GATE2 key-is-(src,seq))
 *   Round 7: Pre-load pending msg for NODE_D + inject ROUTE_RPL (NODE_B→D)
 *            → pending_queue_drain_for_dst re-emits held frame as unicast
 *   Round 8: Pre-set route[NODE_A]=direct, fill TX queue, unicast CHAT to A
 *            → push fails → fallback flood PKT_FLAG_FALLBACK emitted
 * ════════════════════════════════════════════════════════════════════════ */
#ifdef RIVR_SIM_MODE

#define MY_NODE_ID  0xFEED0000ul
#define NODE_A      0xAAAA0001ul
#define NODE_B      0xBBBB0002ul
#define NODE_C      0xCCCC0003ul
#define NODE_D      0xDDDD0004ul   /**< Unknown node for pending-queue test  */

static const char *const s_sim_chat_a[3] = {
    "hello-from-A-0", "hello-from-A-1", "hello-from-A-2",
};
static const char *const s_sim_chat_c[2] = {
    "greet-from-C-0", "greet-from-C-1",
};

/* Push a RIVR binary frame into rf_rx_ringbuf */
static bool sim_push_frame(const rivr_pkt_hdr_t *hdr,
                            const char           *payload,
                            int16_t               rssi_dbm,
                            uint32_t              from_id)
{
    uint8_t plen = payload ? (uint8_t)strlen(payload) : 0u;
    rf_rx_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    int enc = protocol_encode(hdr, (const uint8_t *)payload, plen,
                              frame.data, sizeof(frame.data));
    if (enc < 0) { ESP_LOGW(TAG, "[SIM] encode failed"); return false; }
    frame.len        = (uint8_t)enc;
    frame.rssi_dbm   = rssi_dbm;
    frame.snr_db     = 9;
    frame.rx_mono_ms = tb_millis();
    frame.from_id    = from_id;
    bool ok = rb_try_push(&rf_rx_ringbuf, &frame);
    if (!ok) ESP_LOGW(TAG, "[SIM] ringbuf full");
    return ok;
}

static void sim_inject_packets(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    uint32_t seq = 0;

    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "[SIM] Phase A+D: 3-node mesh demo");
    ESP_LOGI(TAG, "  MY_NODE = 0x%08lx", (unsigned long)MY_NODE_ID);
    ESP_LOGI(TAG, "  NODE_A  = 0x%08lx (direct neighbour)",  (unsigned long)NODE_A);
    ESP_LOGI(TAG, "  NODE_B  = 0x%08lx (relay, direct)",      (unsigned long)NODE_B);
    ESP_LOGI(TAG, "  NODE_C  = 0x%08lx (2-hop via NODE_B)",   (unsigned long)NODE_C);
    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");

    /* Round 1 ── PKT_CHAT from NODE_A (direct, hop=0) ─────────────────── */
    ESP_LOGI(TAG, "[SIM] R1: 3 × PKT_CHAT NODE_A hop=0 (expect RIVR+relay+learn A)");
    for (int i = 0; i < 3; i++, seq++) {
        rivr_pkt_hdr_t h = {
            .magic = RIVR_MAGIC, .version = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT, .ttl = RIVR_PKT_DEFAULT_TTL,
            .src_id = NODE_A, .dst_id = 0, .seq = seq,
        };
        sim_push_frame(&h, s_sim_chat_a[i], -70, NODE_A);
    }

    /* Round 2 ── PKT_CHAT from NODE_C via NODE_B (hop=1) ──────────────── */
    ESP_LOGI(TAG, "[SIM] R2: 2 × PKT_CHAT NODE_C via NODE_B hop=1 (expect learn C→B)");
    for (int i = 0; i < 2; i++, seq++) {
        rivr_pkt_hdr_t h = {
            .magic = RIVR_MAGIC, .version = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT, .flags = PKT_FLAG_RELAY,
            .ttl = RIVR_PKT_DEFAULT_TTL - 1u, .hop = 1,
            .src_id = NODE_C, .dst_id = 0, .seq = seq,
        };
        sim_push_frame(&h, s_sim_chat_c[i], -85, NODE_B);
    }

    /* Round 3 ── PKT_DATA from NODE_A (type=6, RIVR drops via filter) ─── */
    ESP_LOGI(TAG, "[SIM] R3: PKT_DATA NODE_A (expect RIVR-drop, C-relay queued)");
    {
        rivr_pkt_hdr_t h = {
            .magic = RIVR_MAGIC, .version = RIVR_PROTO_VER,
            .pkt_type = PKT_DATA, .ttl = RIVR_PKT_DEFAULT_TTL,
            .src_id = NODE_A, .dst_id = 0, .seq = seq++,
        };
        sim_push_frame(&h, "temp=23.1", -72, NODE_A);
    }

    /* Round 4 ── PKT_ROUTE_REQ from NODE_B asking for MY_NODE ─────────── */
    ESP_LOGI(TAG, "[SIM] R4: ROUTE_REQ from NODE_B dst=MY_NODE (expect ROUTE_RPL)");
    {
        rivr_pkt_hdr_t h = {
            .magic = RIVR_MAGIC, .version = RIVR_PROTO_VER,
            .pkt_type = PKT_ROUTE_REQ, .ttl = ROUTE_REQ_TTL,
            .src_id = NODE_B, .dst_id = MY_NODE_ID, .seq = seq++,
        };
        sim_push_frame(&h, NULL, -80, NODE_B);
    }

    /* Round 5 ── Duplicate of Round-1 frame 0 via same relay ───────────── *
     * GATE2 probe 1: same (src_id=NODE_A, seq=0), same from_id=NODE_A.      *
     * Proves basic dedupe drop.                                               */
    ESP_LOGI(TAG, "[SIM] R5: R1[0] exact duplicate same relay  (expect DEDUPE-DROP)");
    {
        rivr_pkt_hdr_t h = {
            .magic = RIVR_MAGIC, .version = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT, .ttl = RIVR_PKT_DEFAULT_TTL,
            .src_id = NODE_A, .dst_id = 0, .seq = 0, /* same (src,seq) as R1[0] */
        };
        sim_push_frame(&h, "hello-from-A-0", -68, NODE_A);
    }

    /* Round 6 ── GATE2: same (src,seq) via DIFFERENT relay ────────────────── *
     * NODE_A's original frame (seq=0) is now re-forwarded by NODE_B.         *
     * key insight: dedupe is on (src_id, seq) — from_id is irrelevant.       *
     * Expected: DEDUPE-DROP even though from_id changed.                      */
    ESP_LOGI(TAG, "[SIM] R6: R1[0] same (src,seq) different relay NODE_B (expect DEDUPE-DROP)");
    {
        rivr_pkt_hdr_t h = {
            .magic = RIVR_MAGIC, .version = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT, .flags = PKT_FLAG_RELAY,
            .ttl = RIVR_PKT_DEFAULT_TTL - 1u, .hop = 1,
            .src_id = NODE_A, .dst_id = 0, .seq = 0, /* same (src,seq) as R1[0] */
        };
        /* from_id = NODE_B — different relay, but (src_id=NODE_A, seq=0)
         * is already in the dedupe ring. Must be dropped. */
        sim_push_frame(&h, "hello-from-A-0", -75, NODE_B);
    }

    ESP_LOGI(TAG, "[SIM] %u frames queued — main loop will process",
             rf_rx_ringbuf.cap - rb_available(&rf_rx_ringbuf));

    /* ──────────────────────────────────────────────────────────────────────── *
     * These direct-function tests run synchronously in sim_inject_packets()  *
     * before the main loop starts. They exercise the TX-path and pending      *
     * queue by manipulating state directly, then letting the main loop show   *
     * the resulting log output.                                                *
     * ──────────────────────────────────────────────────────────────────────── */

    /* Round 7 ── Pending queue + ROUTE_RPL drain ────────────────────────── *
     * Pre-load a pending message for NODE_D (simulates a RIVR unicast TX     *
     * that hit a cache miss in rf_tx_sink_cb).  Then inject a ROUTE_RPL from *
     * NODE_B saying “I can reach NODE_D in 2 hops”.  When main loop processes *
     * the RPL, pending_queue_drain_for_dst() should flush the message.        */
    ESP_LOGI(TAG, "[SIM] R7: pending-queue drain (pre-load msg for NODE_D + inject ROUTE_RPL)");
    {
        uint32_t now_ms = tb_millis();

        /* Encode a dummy outgoing CHAT from MY_NODE to NODE_D */
        rivr_pkt_hdr_t ph = {
            .magic    = RIVR_MAGIC, .version  = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT,   .ttl      = RIVR_PKT_DEFAULT_TTL,
            .src_id   = MY_NODE_ID, .dst_id   = NODE_D, .seq = seq++,
        };
        uint8_t pd[64] = {0};
        int pl = protocol_encode(&ph, (const uint8_t *)"waiting-for-route", 17, pd, sizeof(pd));
        if (pl > 0) {
            bool pq_ok = pending_queue_enqueue(
                &g_pending_queue, NODE_D,
                pd, (uint8_t)pl, RF_TOA_APPROX_US((uint8_t)pl), now_ms);
            ESP_LOGI(TAG, "  pre-loaded pending[NODE_D]: %s",
                     pq_ok ? "ok" : "FAILED-queue-full");
        }

        /* Inject ROUTE_RPL: NODE_B replies saying target=NODE_D via itself, 1 hop */
        uint8_t rpl_buf[64] = {0};
        int rpl_len = routing_build_route_rpl(
            NODE_B,       /* sender of the RPL */
            MY_NODE_ID,   /* we asked the question */
            NODE_D,       /* the target we wanted */
            NODE_B,       /* next_hop from B's perspective = B itself (it IS a neighbour of D) */
            1u,           /* hop_count: B is 1 hop from D */
            seq++,
            rpl_buf, sizeof(rpl_buf));
        if (rpl_len > 0) {
            rivr_pkt_hdr_t rpl_h;
            const uint8_t *rpl_pl = NULL;
            if (protocol_decode(rpl_buf, (uint8_t)rpl_len, &rpl_h, &rpl_pl)) {
                sim_push_frame(&rpl_h, (const char *)rpl_pl, -82, NODE_B);
                ESP_LOGI(TAG, "  injected ROUTE_RPL from NODE_B for NODE_D");
            }
        }
    }

    /* Round 8 ── Unicast failover (TX queue full → PKT_FLAG_FALLBACK) ─────── *
     * Pre-populate route_cache so rf_tx_sink_cb picks UNICAST for NODE_A.   *
     * Fill rf_tx_queue with dummy frames to force a push failure.            *
     * Then call rf_tx_sink_cb directly — it should detect UNICAST + fail    *
     * and emit a fallback flood with PKT_FLAG_FALLBACK.                      */
    ESP_LOGI(TAG, "[SIM] R8: unicast-failover (queue full → PKT_FLAG_FALLBACK)");
    {
        uint32_t now_ms = tb_millis();

        /* Make sure NODE_A has a direct route (it should already from R1, but
         * R1 hasn't been processed yet at this point — pre-populate manually) */
        route_cache_update(&g_route_cache, NODE_A, NODE_A,
                           /*hops*/1u, /*metric*/185u,
                           RCACHE_FLAG_VALID | RCACHE_FLAG_DIRECT,
                           now_ms);
        ESP_LOGI(TAG, "  pre-set route[NODE_A]=direct");

        /* Fill rf_tx_queue with dummies to exhaust capacity */
        uint8_t filled = 0u;
        for (uint8_t f = 0u; f < RF_TX_QUEUE_CAP; f++) {
            rf_tx_request_t dummy;
            memset(&dummy, 0, sizeof(dummy));
            dummy.data[0] = 0xFFu;
            dummy.len     = 1u;
            dummy.toa_us  = 1000u;
            if (rb_try_push(&rf_tx_queue, &dummy)) filled++;
        }
        ESP_LOGI(TAG, "  TX queue filled with %u dummies (cap=%u)", filled, RF_TX_QUEUE_CAP);

        /* Build a unicast CHAT from MY_NODE to NODE_A, then emit via sink */
        rivr_pkt_hdr_t uh = {
            .magic    = RIVR_MAGIC,  .version  = RIVR_PROTO_VER,
            .pkt_type = PKT_CHAT,    .ttl      = RIVR_PKT_DEFAULT_TTL,
            .src_id   = MY_NODE_ID,  .dst_id   = NODE_A,  .seq = seq++,
        };
        uint8_t ud[64] = {0};
        int ul = protocol_encode(&uh, (const uint8_t *)"hi-A-retry", 10, ud, sizeof(ud));
        if (ul > 0) {
            rivr_value_t rv;
            memset(&rv, 0, sizeof(rv));
            rv.tag = RIVR_VAL_BYTES;
            uint16_t copy = (uint16_t)ul < sizeof(rv.as_bytes.buf)
                            ? (uint16_t)ul : sizeof(rv.as_bytes.buf);
            memcpy(rv.as_bytes.buf, ud, copy);
            rv.as_bytes.len = copy;
            /* Expected log: rf_tx: unicast dst=NODE_A via next_hop=NODE_A
             *               rf_tx: unicast queue full → FALLBACK flood ... */
            rf_tx_sink_cb(&rv, NULL);
        }
        /* Note: the queue still holds dummies; tx_drain_loop will flush them
         * during the first few main-loop iterations. */
    }
}

/** Initialise ring-buffers without touching SPI/GPIO.
 *  TODO(SX1262): replace with radio_init() once hardware is connected. */
static inline void radio_sim_init(void)
{
    radio_init_buffers_only();
}

#endif /* RIVR_SIM_MODE */

/* ── TX drain: pop from rf_tx_queue, gate on duty cycle, transmit ───────── */

static void tx_drain_loop(void)
{
    uint32_t now_ms = tb_millis();

    for (uint32_t i = 0; i < TX_DRAIN_LIMIT; i++) {
        rf_tx_request_t req;
        if (!rb_pop(&rf_tx_queue, &req)) break;   /* queue empty */

        /* ── Jitter gate: hold frame until its due_ms timestamp ──────────── *
         * due_ms == 0 means "send immediately".                             *
         * If the head frame isn't ready, push it back to the TAIL of the   *
         * queue and CONTINUE to the next iteration — do NOT break.          *
         * Breaking would cause head-of-line blocking: an immediate frame    *
         * sitting behind a deferred one would never drain this tick.         *
         * Using continue is safe because TX_DRAIN_LIMIT bounds total pops.  */
        if (req.due_ms != 0u && req.due_ms > now_ms) {
            if (!rb_try_push(&rf_tx_queue, &req)) {
                /* Should never happen (we just freed a slot), but don't drop */
                ESP_LOGW(TAG, "TX re-push failed for jittered frame – dropped");
            }
            continue;   /* skip — try next frame; revisit this one next tick  */
        }

        /* Hard duty-cycle gate (C-layer backup; RIVR budget.toa_us already
           filtered the pipeline, but this is the final hardware guard) */
        if (!dutycycle_check(&g_dc, now_ms, req.toa_us)) {
            ESP_LOGW(TAG, "TX frame dropped by C duty-cycle gate (toa=%u us)", req.toa_us);
            continue;
        }

#ifdef RIVR_SIM_TX_PRINT
        /* ── Simulation TX path ─────────────────────────────────────────── *
         * Decode the binary frame and log what would be transmitted.        *
         * TODO(SX1262): remove #ifdef and always use radio_transmit() once  *
         * the real radio driver is verified.                                 *
         * ────────────────────────────────────────────────────────────────── */
        {
            rivr_pkt_hdr_t pkt;
            const uint8_t *pl = NULL;
            bool ok = protocol_decode(req.data, req.len, &pkt, &pl);

            if (ok) {
                const char *type_str =
                    (pkt.pkt_type == PKT_CHAT)      ? "CHAT"      :
                    (pkt.pkt_type == PKT_BEACON)    ? "BEACON"    :
                    (pkt.pkt_type == PKT_DATA)      ? "DATA"      :
                    (pkt.pkt_type == PKT_ROUTE_REQ) ? "ROUTE_REQ" :
                    (pkt.pkt_type == PKT_ROUTE_RPL) ? "ROUTE_RPL" :
                    (pkt.pkt_type == PKT_ACK)       ? "ACK"       : "UNKN";

                /* Print payload as a null-terminated string if it is printable */
                char payload_str[RIVR_PKT_MAX_PAYLOAD + 1] = {0};
                if (pl && pkt.payload_len > 0) {
                    uint8_t n = pkt.payload_len < RIVR_PKT_MAX_PAYLOAD
                                ? pkt.payload_len : RIVR_PKT_MAX_PAYLOAD;
                    memcpy(payload_str, pl, n);
                    payload_str[n] = '\0';
                }

                ESP_LOGI(TAG,
                    "[SIM] TX type=%s src=0x%08lx seq=%lu ttl=%u hop=%u "
                    "len=%u payload=\"%s\" toa=%u us",
                    type_str,
                    (unsigned long)pkt.src_id,
                    (unsigned long)pkt.seq,
                    pkt.ttl, pkt.hop,
                    req.len, payload_str, req.toa_us);
            } else {
                ESP_LOGI(TAG,
                    "[SIM] TX <non-RIVR frame> len=%u toa=%u us",
                    req.len, req.toa_us);
            }
        }
        dutycycle_record(&g_dc, tb_millis(), req.toa_us);
#else
        /* ── Real hardware TX path ──────────────────────────────────────── */
        platform_led_on();
        bool tx_ok = radio_transmit(&req);
        platform_led_off();

        if (tx_ok) {
            dutycycle_record(&g_dc, tb_millis(), req.toa_us);
            ESP_LOGD(TAG, "TX ok: %u bytes, toa=%u us", req.len, req.toa_us);
        } else {
            ESP_LOGE(TAG, "TX failed");
        }
#endif  /* RIVR_SIM_TX_PRINT */
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "═══ RIVR Embedded Node booting ═══");
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());
#ifdef RIVR_SIM_MODE
    ESP_LOGI(TAG, "*** SIMULATION MODE: no real SX1262 hardware ***");
#endif

    /* ── Hardware init ── */
#ifdef RIVR_SIM_MODE
    /* In sim mode skip full platform_init (no SPI bus, no GPIO interrupts).
     * Only initialise the ring-buffers inside the radio module so that
     * sim_inject_packets() can push frames immediately.
     * TODO(SX1262): replace with platform_init() once hardware is connected. */
    timebase_init();
    dutycycle_init(&g_dc);
    radio_sim_init();   /* initialises ringbufs, attempts SX1262 reset (safe to ignore) */
#else
    platform_init();
    timebase_init();
    radio_init();
    dutycycle_init(&g_dc);

    /* Derive unique node ID from lower 4 bytes of WiFi STA MAC address */
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        g_my_node_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
                     | ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];
        ESP_LOGI(TAG, "Node ID: 0x%08lX (from MAC bytes [2..5])",
                 (unsigned long)g_my_node_id);
    }
#endif

#ifdef RIVR_SIM_MODE
    /* Assign our simulated node identity before the RIVR engine starts so
     * routing_should_reply_route_req(), route_cache_learn_rx(), etc., can
     * distinguish this node from the injected remote nodes. */
    g_my_node_id = MY_NODE_ID;
    ESP_LOGI(TAG, "[SIM] node identity: 0x%08lx", (unsigned long)MY_NODE_ID);
#endif

    /* ── RIVR engine init ── */
    /* Publish identity globals before init so beacon_sink_cb and NVS helpers
       can read callsign and net_id from the moment the engine starts. */
    g_net_id = (uint16_t)RIVR_NET_ID;
    strncpy(g_callsign, RIVR_CALLSIGN, sizeof(g_callsign) - 1u);
    g_callsign[sizeof(g_callsign) - 1u] = '\0';
    rivr_embed_init();

    /* ── Display task (spawns low-priority FreeRTOS task; never blocks main) ── */
    {
        display_stats_t boot_stats;
        memset(&boot_stats, 0, sizeof(boot_stats));
        boot_stats.node_id = g_my_node_id;
        boot_stats.net_id  = (uint16_t)RIVR_NET_ID;
        strncpy(boot_stats.callsign, RIVR_CALLSIGN, sizeof(boot_stats.callsign) - 1u);
#ifndef RIVR_SIM_MODE
        display_task_start(&boot_stats);
#endif
    }

#ifdef RIVR_SIM_MODE
    /* Push simulated frames AFTER engine is ready so clock state is fully
     * initialised, but BEFORE radio_start_rx (which is skipped in sim mode). */
    sim_inject_packets();
    ESP_LOGI(TAG, "[SIM] frames injected, entering main loop (rivr_tick will process them)");
#else
    /* ── Start radio RX ── */
    radio_start_rx();
#endif

    ESP_LOGI(TAG, "entering main loop");

    uint32_t last_stats_ms = 0;
    uint32_t loop_count    = 0;
    display_stats_t disp;       /* stats snapshot updated each iteration */
    memset(&disp, 0, sizeof(disp));
    strncpy(disp.callsign, RIVR_CALLSIGN, sizeof(disp.callsign) - 1u);
    disp.net_id = (uint16_t)RIVR_NET_ID;

    for (;;) {
        /* ─ 1. RIVR processing tick ─ */
        rivr_tick();

        /* ─ 2. TX drain (with duty-cycle gate) ─ */
        tx_drain_loop();

        /* ─ 2b. Hot-reload check: if PKT_PROG_PUSH stored a new program ─ */
        if (g_program_reload_pending) {
            rivr_embed_reload();
        }

        /* ─ 3. Periodic diagnostics ─ */
        uint32_t now = tb_millis();
        if (now - last_stats_ms >= STATS_INTERVAL_MS) {
            last_stats_ms = now;
            ESP_LOGI(TAG, "── stats (loop#%lu, uptime=%lu ms) ──",
                     (unsigned long)loop_count, (unsigned long)now);
            ESP_LOGI(TAG, "  rx_ringbuf available : %u", rb_available(&rf_rx_ringbuf));
            ESP_LOGI(TAG, "  rx_drops             : %u", rb_drops(&rf_rx_ringbuf));
            ESP_LOGI(TAG, "  tx_queue  available  : %u", rb_available(&rf_tx_queue));
            ESP_LOGI(TAG, "  tx_drops             : %u", rb_drops(&rf_tx_queue));
            rivr_embed_print_stats();
        }

        /* ─ 4. Publish display stats snapshot (task picks it up at its own pace) ─ */
#ifndef RIVR_SIM_MODE
        {
            /* Fill the snapshot from live globals — cheap reads, no alloc */
            disp.node_id        = g_my_node_id;
            disp.uptime_s       = now / 1000u;
            disp.rssi_inst_dbm  = radio_get_rssi_inst();
            disp.rssi_dbm       = g_last_rssi_dbm;
            disp.snr_db         = g_last_snr_db;
            disp.rx_count       = g_rx_frame_count;
            disp.tx_count       = g_tx_frame_count;
            disp.neighbor_count = routing_neighbor_count(&g_neighbor_table, now);
            disp.route_count    = route_cache_count(&g_route_cache, now);
            disp.pending_count  = g_pending_queue.count;
            /* Duty cycle used: accumulate expressed as ×10 percentage */
            uint64_t dc_used_us = g_dc.used_us;
            disp.dc_used_pct_x10 = (DC_BUDGET_US > 0u)
                ? (uint16_t)(dc_used_us * 1000u / DC_BUDGET_US) : 0u;
            disp.dc_backoff_ms  = 0u;  /* no explicit backoff timer yet */
            disp.vm_cycles      = g_vm_total_cycles;
            disp.error_code     = g_vm_last_error;
            /* last_event: not yet plumbed — leave as "" */
            display_post_stats(&disp);
        }
#endif

        loop_count++;

        /* ─ 5. Yield 10 ms — long enough to be ≥1 tick at any FreeRTOS tick rate.
         *    pdMS_TO_TICKS(1) truncates to 0 at 100 Hz (the default); using 10 ms
         *    ensures IDLE0 always gets CPU time, preventing Task WDT triggers. ─ */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
