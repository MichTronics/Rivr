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
#include <inttypes.h>   /* PRIu32 */
#include "rivr_programs/default_program.h"
#include "../firmware_core/radio_sx1262.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../firmware_core/dutycycle.h"
#include "../firmware_core/timebase.h"
#include "../firmware_core/routing.h"
#include "../firmware_core/route_cache.h"
#include "../firmware_core/pending_queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "../firmware_core/rivr_log.h"
#include "../firmware_core/rivr_policy.h"

/* Weak stub — overridden by strong symbol from Rust rivr_core when
 * compiled with --features ffi.  Allows linking even against a
 * librivr_core.a that predates alloc_guard.rs.                   */
void __attribute__((weak)) rivr_runtime_freeze_alloc(void) {}

#define TAG             "RIVR_EMBED"
#define MAX_ENGINE_STEPS  256u   /* max scheduler cycles per rivr_tick() call */

/* ── Shared routing state ────────────────────────────────────────────────── */
route_cache_t   g_route_cache;      /* Phase-D hybrid unicast route table        */
pending_queue_t g_pending_queue;    /* Phase-D pending messages for route miss   */
uint32_t        g_my_node_id = 0;   /* Set by platform / sim before embed_init() */
uint32_t        g_ctrl_seq   = 0;   /* Sequence counter for ROUTE_REQ/RPL frames */
/* ── Display/diagnostic stats counters ────────────────────────────────────── */
uint32_t        g_rx_frame_count = 0u;  /* updated by rivr_sources.c */
uint32_t        g_tx_frame_count = 0u;  /* updated by rivr_sinks.c   */
int16_t         g_last_rssi_dbm  = 0;   /* RSSI of most recent RX frame */
int8_t          g_last_snr_db    = 0;   /* SNR  of most recent RX frame */
uint32_t        g_vm_total_cycles = 0u; /* accumulated from rivr_engine_run() */
uint32_t        g_vm_last_error   = 0u; /* last non-OK result code */
neighbor_table_t      g_neighbor_table;     /* updated by rivr_sources.c */
rivr_neighbor_table_t g_ntable;             /* standalone quality tracker */
/* ── Node identity (set by main.c before rivr_embed_init) ───────────────── */
char             g_callsign[12]           = "RIVR";
uint16_t         g_net_id                 = 0u;
bool             g_program_reload_pending = false;

/* ── NVS program storage ─────────────────────────────────────────────────── */

#define RIVR_NVS_NAMESPACE  "rivr"
#define RIVR_NVS_KEY_PROG   "program"
#define RIVR_NVS_PROG_MAX   2048u

static char s_nvs_program[RIVR_NVS_PROG_MAX];

/* Buffer for dynamically-generated policy program (from g_policy_params). */
#define RIVR_POLICY_PROG_MAX  512u
static char s_policy_buf[RIVR_POLICY_PROG_MAX];

static const char *nvs_load_program(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RIVR_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return NULL;
    size_t len = sizeof(s_nvs_program);
    err = nvs_get_str(h, RIVR_NVS_KEY_PROG, s_nvs_program, &len);
    nvs_close(h);
    if (err == ESP_OK && len > 1u) {
        RIVR_LOGI(TAG, "NVS program loaded (%u bytes)", (unsigned)len);
        return s_nvs_program;
    }
    return NULL;
}

bool rivr_nvs_store_program(const char *src)
{
    if (!src) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(RIVR_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %d", (int)err);
        return false;
    }
    err = nvs_set_str(h, RIVR_NVS_KEY_PROG, src);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %d", (int)err);
        return false;
    }
    RIVR_LOGI(TAG, "NVS program stored (%u bytes)", (unsigned)strlen(src));
    return true;
}

/* ── NVS identity storage ────────────────────────────────────────────────── */

#define RIVR_NVS_KEY_CALLSIGN  "callsign"
#define RIVR_NVS_KEY_NETID     "netid"

void rivr_nvs_load_identity(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RIVR_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return;  /* NVS not yet provisioned — keep compile-time defaults */

    /* Callsign: stored as NUL-terminated string, max 11 chars + NUL */
    char cs[12];
    size_t len = sizeof(cs);
    if (nvs_get_str(h, RIVR_NVS_KEY_CALLSIGN, cs, &len) == ESP_OK && len > 1u) {
        strncpy(g_callsign, cs, sizeof(g_callsign) - 1u);
        g_callsign[sizeof(g_callsign) - 1u] = '\0';
        RIVR_LOGI(TAG, "NVS callsign loaded: %s", g_callsign);
    }

    /* Net ID: stored as 16-bit unsigned integer */
    uint16_t nid = 0u;
    if (nvs_get_u16(h, RIVR_NVS_KEY_NETID, &nid) == ESP_OK) {
        g_net_id = nid;
        RIVR_LOGI(TAG, "NVS net_id loaded: 0x%04X", (unsigned)g_net_id);
    }

    nvs_close(h);
}

bool rivr_nvs_store_identity(const char *callsign, uint16_t net_id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RIVR_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open (identity) failed: %d", (int)err);
        return false;
    }

    bool ok = true;
    if (callsign && callsign[0] != '\0') {
        err = nvs_set_str(h, RIVR_NVS_KEY_CALLSIGN, callsign);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS set callsign failed: %d", (int)err);
            ok = false;
        }
    }

    err = nvs_set_u16(h, RIVR_NVS_KEY_NETID, net_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set netid failed: %d", (int)err);
        ok = false;
    }

    if (ok) { err = nvs_commit(h); }
    nvs_close(h);

    if (ok && err == ESP_OK) {
        RIVR_LOGI(TAG, "NVS identity stored: %s / 0x%04X",
                 callsign ? callsign : "(unchanged)", (unsigned)net_id);
        return true;
    }
    ESP_LOGE(TAG, "NVS identity commit failed: %d", (int)err);
    return false;
}

/* u64 trampoline: rivr_foreach_timer_source gives uint64_t; sources API uses uint32_t */
static void s_timer_reg_cb(const char *name, uint64_t interval_ms)
{
    uint32_t ms32 = (interval_ms > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)interval_ms;
    sources_register_timer(name, ms32);
}

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

    /* 3. Fire any due timer sources */
    total += sources_timer_drain();

    /* 4. Run engine – processes pending events, fires emit callbacks */
    rivr_result_t run = rivr_engine_run(MAX_ENGINE_STEPS);
    total += run.cycles_used;
    g_vm_total_cycles += run.cycles_used;
    if (run.code != RIVR_OK) {
        g_vm_last_error = run.code;
    }
    if (run.gas_remaining == 0) {
        /* gas_remaining == 0 means the scheduler was NOT idle at return.
         * This may indicate an unbounded program or tick-storm on the input. */
        ESP_LOGW(TAG, "rivr_engine_run gas exhausted (%" PRIu32 " steps) – starvation risk",
                 run.cycles_used);
    }

    return total;
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void rivr_embed_init(void)
{
    RIVR_LOGI(TAG, "rivr_embed_init: loading default program");

    /* Initialise C-layer routing state (dedupe cache, forward budget) */
    routing_init();
    routing_neighbor_init(&g_neighbor_table);
    neighbor_table_init(&g_ntable);
    route_cache_init(&g_route_cache);
    pending_queue_init(&g_pending_queue);
    ESP_LOGD(TAG, "routing + route_cache + pending_queue + neighbor_table initialised");

    /* Register sinks before engine init (callbacks must be ready before
       any emit can fire) */
    rivr_sinks_init();
    rivr_sources_init();

    /* ── Wire the C emit-dispatch callback into the Rust FFI layer.
     *    CRITICAL: must be called before rivr_engine_init() so that any
     *    emits fired during first engine.run() reach the C sinks.       ── */
    rivr_set_emit_dispatch(rivr_emit_dispatch);
    ESP_LOGD(TAG, "rivr_emit_dispatch wired to Rust engine");

    /* Compile and load the RIVR program: try NVS first, fall back to
       compiled-in default on first boot or after flash. */
    const char *prog_src = nvs_load_program();
    if (!prog_src) {
        rivr_policy_build_program(s_policy_buf, sizeof(s_policy_buf));
        prog_src = s_policy_buf;
        RIVR_LOGI(TAG, "rivr_embed_init: no NVS program – using policy defaults");
    }
    rivr_result_t rc = rivr_engine_init(prog_src);
    if (rc.code != RIVR_OK) {
        ESP_LOGE(TAG, "rivr_engine_init failed: code %" PRIu32 " (RIVR_ERR_* constants in rivr_embed.h)",
                 rc.code);
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

#if !RIVR_SIM_MODE
    /* Mark runtime-init boundary — no heap allocation expected after this. */
    rivr_runtime_freeze_alloc();
#endif

    /* Enumerate timer sources from compiled graph and register with C driver */
    rivr_foreach_timer_source(s_timer_reg_cb);

    RIVR_LOGI(TAG, "rivr_embed_init: engine ready");
#if RIVR_SIM_MODE
    RIVR_LOGI(TAG, "SIM program:\n%s", RIVR_SIM_PROGRAM);
#else
    RIVR_LOGI(TAG, "program:\n%s", RIVR_DEFAULT_PROGRAM);
#endif
}

void rivr_embed_print_stats(void)
{
    /* TODO: call rivr_engine_print_stats() FFI when implemented */
    RIVR_LOGI(TAG, "clock[0]=%llu clock[1]=%llu",
             (unsigned long long)rivr_engine_clock_now(0),
             (unsigned long long)rivr_engine_clock_now(1));
    dutycycle_print_stats(&g_dc);
}

bool rivr_embed_reload(void)
{
    RIVR_LOGI(TAG, "rivr_embed_reload: reloading engine");

    /* Clear timer registrations so they are re-populated from new program */
    sources_timer_reset();

    /* Load new program (NVS or default) */
    const char *prog_src = nvs_load_program();
    if (!prog_src) {
        rivr_policy_build_program(s_policy_buf, sizeof(s_policy_buf));
        prog_src = s_policy_buf;
        RIVR_LOGI(TAG, "rivr_embed_reload: NVS empty – rebuilding from policy params");
    }

    rivr_result_t rc = rivr_engine_init(prog_src);
    if (rc.code != RIVR_OK) {
        ESP_LOGE(TAG, "rivr_embed_reload: engine init failed code=%" PRIu32, rc.code);
        g_program_reload_pending = false;
        return false;
    }

    /* Re-register timer sources from new compiled graph */
    rivr_foreach_timer_source(s_timer_reg_cb);

    g_program_reload_pending = false;
    rivr_policy_notify_reload();   /* increment policy_reload_count */
    RIVR_LOGI(TAG, "rivr_embed_reload: done");
    return true;
}
