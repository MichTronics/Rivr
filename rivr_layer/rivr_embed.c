/**
 * @file  rivr_embed.c
 * @brief Glue between firmware_core and rivr_core: init + rivr_tick().
 *
 * ARCHITECTURE DIAGRAM
 * ───────────────────────────────────────────────────────────────────
 *
 *   ISR ─────────────────────────────────────► rf_rx_ringbuf
 *                                                    │
 *   main loop ──► rivr_tick()                        │
 *                    │                               │
 *                    ├── sources_rf_rx_drain()  ◄────┘  (pop frames)
 *                    │      └─ rivr_inject_event("rf_rx", ev)
 *                    │                               ▼
 *                    ├── rivr_engine_run(steps) ── RUST rivr_core ──► emit
 *                    │                                                  │
 *                    │     ┌────────────────────────────────────────────┘
 *                    │     ▼
 *                    └── rf_tx_sink_cb() ──► rf_tx_queue
 *                                                │
 *   main loop (after rivr_tick()) ───────────────┘
 *       ├── dutycycle_check()
 *       └── radio_transmit()
 *
 * MEMORY BUDGET
 * ─────────────
 *  rivr_core engine state:   ~4 KB (static, in Rust BSS)
 *  rf_rx_ringbuf (8 × 258B): ~2 KB
 *  rf_tx_queue  (4 × 258B):  ~1 KB
 *  dc_ctx_t + history (64×8): 512 B
 *  ──────────────────────────────
 *  Total extra RAM:           < 8 KB  (well within ESP32's 320 KB DRAM)
 */

#include "rivr_embed.h"
#include "rivr_sources.h"
#include "rivr_sinks.h"
#include "rivr_programs/default_program.h"
#include "../firmware_core/radio_sx1262.h"
#include "../firmware_core/dutycycle.h"
#include "../firmware_core/timebase.h"
#include "../firmware_core/routing.h"
#include "../firmware_core/route_cache.h"
#include "../firmware_core/pending_queue.h"
#include "esp_log.h"

#define TAG             "RIVR_EMBED"
#define MAX_ENGINE_STEPS  256u   /* max scheduler cycles per rivr_tick() call */

/* ── Shared routing state ────────────────────────────────────────────────── */
route_cache_t   g_route_cache;      /* Phase-D hybrid unicast route table        */
pending_queue_t g_pending_queue;    /* Phase-D pending messages for route miss   */
uint32_t        g_my_node_id = 0;   /* Set by platform / sim before embed_init() */
uint32_t        g_ctrl_seq   = 0;   /* Sequence counter for ROUTE_REQ/RPL frames */

/* ── Sink registration table ─────────────────────────────────────────────── *
 * Simple linear table; 8 slots is more than enough for embedded use.
 * ─────────────────────────────────────────────────────────────────────────── */
#define MAX_SINK_REGS  8u

typedef struct {
    const char    *name;
    rivr_sink_cb_t cb;
    void          *ctx;
} sink_reg_t;

static sink_reg_t s_sinks[MAX_SINK_REGS];
static uint8_t    s_sink_count = 0;

void rivr_register_sink(const char *sink_name, rivr_sink_cb_t cb, void *user_ctx)
{
    if (s_sink_count >= MAX_SINK_REGS) {
        ESP_LOGE(TAG, "rivr_register_sink: table full");
        return;
    }
    s_sinks[s_sink_count].name = sink_name;
    s_sinks[s_sink_count].cb   = cb;
    s_sinks[s_sink_count].ctx  = user_ctx;
    s_sink_count++;
    ESP_LOGD(TAG, "registered sink: %s", sink_name);
}

/* ── Called by the Rust FFI layer when RIVR fires an emit ────────────────── *
 * This function is `#[no_mangle] extern "C"` from the C side; the Rust FFI  *
 * calls it as a function pointer stored in the engine context.               *
 * ─────────────────────────────────────────────────────────────────────────── */
void rivr_emit_dispatch(const char *sink_name, const rivr_value_t *v)
{
    for (uint8_t i = 0; i < s_sink_count; i++) {
        if (strcmp(s_sinks[i].name, sink_name) == 0) {
            s_sinks[i].cb(v, s_sinks[i].ctx);
            return;
        }
    }
    ESP_LOGW(TAG, "emit to unknown sink: %s", sink_name);
}

/* ── rivr_tick ───────────────────────────────────────────────────────────── */

uint32_t rivr_tick(void)
{
    uint32_t total = 0;

    /* 1. Pull RX frames from radio ringbuf and inject into RIVR */
    total += sources_rf_rx_drain();

    /* 2. Optionally inject CLI events */
    total += sources_cli_drain();

    /* 3. Run engine – processes pending events, fires emit callbacks */
    uint32_t steps = rivr_engine_run(MAX_ENGINE_STEPS);
    total += steps;

    return total;
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void rivr_embed_init(void)
{
    ESP_LOGI(TAG, "rivr_embed_init: loading default program");

    /* Initialise C-layer routing state (dedupe cache, forward budget) */
    routing_init();
    route_cache_init(&g_route_cache);
    pending_queue_init(&g_pending_queue);
    ESP_LOGD(TAG, "routing + route_cache + pending_queue initialised");

    /* Register sinks before engine init (callbacks must be ready before
       any emit can fire) */
    rivr_sinks_init();
    rivr_sources_init();

    /* ── Wire the C emit-dispatch callback into the Rust FFI layer.
     *    CRITICAL: must be called before rivr_engine_init() so that any
     *    emits fired during first engine.run() reach the C sinks.       ── */
    rivr_set_emit_dispatch(rivr_emit_dispatch);
    ESP_LOGD(TAG, "rivr_emit_dispatch wired to Rust engine");

    /* Compile and load the default RIVR program from flash.
       In RIVR_SIM_MODE, RIVR_ACTIVE_PROGRAM also emits to usb_print for
       immediate UART visibility.  On hardware RIVR_ACTIVE_PROGRAM = RIVR_DEFAULT_PROGRAM. */
    int32_t rc = rivr_engine_init(RIVR_ACTIVE_PROGRAM);
    if (rc != 0) {
        ESP_LOGE(TAG, "rivr_engine_init failed: %d", rc);
        /* Halt – firmware cannot operate without the RIVR engine */
        for (;;) { }
    }

    ESP_LOGI(TAG, "rivr_embed_init: engine ready");
#ifdef RIVR_SIM_MODE
    ESP_LOGI(TAG, "SIM program:\n%s", RIVR_SIM_PROGRAM);
#else
    ESP_LOGI(TAG, "program:\n%s", RIVR_DEFAULT_PROGRAM);
#endif
}

void rivr_embed_print_stats(void)
{
    /* TODO: call rivr_engine_print_stats() FFI when implemented */
    ESP_LOGI(TAG, "clock[0]=%llu clock[1]=%llu",
             (unsigned long long)rivr_engine_clock_now(0),
             (unsigned long long)rivr_engine_clock_now(1));
    dutycycle_print_stats(&g_dc);
}
