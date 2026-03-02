/*
 * rivr_policy.h — Runtime-adjustable Rivr policy parameters.
 *
 * Rivr Policy Parameters are now runtime-adjustable via PKT_PROG_PUSH.
 * Mechanism (routing, ACK, queue) remains in C.
 * Rivr continues to serve as policy layer only.
 *
 * Default values are compiled in via RIVR_PARAM_* macros (default_program.h).
 * Parameters can be updated at runtime by receiving a PKT_PROG_PUSH payload
 * that begins with "@PARAMS " (see rivr_sources.c for the handler).
 *
 * Wire format for param-update payload:
 *   @PARAMS beacon=<ms> chat=<ms> data=<ms> duty=<1..10>
 *   Example: "@PARAMS beacon=60000 chat=2000 data=2000 duty=5"
 *
 * On receiving an @PARAMS payload:
 *   - g_policy_params is updated with validated values.
 *   - A new RIVR program is built from g_policy_params and stored to NVS.
 *   - The engine reloads the new program at the next main-loop iteration.
 *
 * If no @PARAMS payload has been received since boot, behavior is identical
 * to the previous firmware release (no NVS program, RIVR_ACTIVE_PROGRAM used).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Policy parameter struct ─────────────────────────────────────────────── */

/**
 * Runtime-adjustable Rivr policy parameters.
 * Updated via rivr_policy_set_param(); read by rivr_policy_build_program().
 */
typedef struct {
    uint32_t beacon_interval_ms;   /**< Beacon timer period, ms (min 1000)  */
    uint32_t chat_throttle_ms;     /**< PKT_CHAT stream throttle window, ms */
    uint32_t data_throttle_ms;     /**< PKT_DATA stream throttle window, ms */
    uint8_t  duty_percent;         /**< TX duty-cycle limit 1–10 %          */
} rivr_policy_params_t;

/** Param IDs used with rivr_policy_set_param(). */
typedef enum {
    RIVR_PARAM_ID_BEACON_INTERVAL = 1,
    RIVR_PARAM_ID_CHAT_THROTTLE   = 2,
    RIVR_PARAM_ID_DATA_THROTTLE   = 3,
    RIVR_PARAM_ID_DUTY_PERCENT    = 4,
} rivr_param_id_t;

/** Global policy parameter state — read-only outside rivr_policy.c. */
extern rivr_policy_params_t g_policy_params;

/* ── Policy metrics struct ───────────────────────────────────────────────── */

/**
 * Counters that track how the policy layer has been exercised at runtime.
 * All fields are monotonically increasing (wrap at UINT32_MAX).
 * Read-only via rivr_policy_metrics_get(); never expose the global directly.
 */
typedef struct {
    uint32_t params_update_count;       /**< Successful rivr_policy_set_param() calls      */
    uint32_t last_params_update_uptime_ms; /**< tb_millis() at last successful param update */
    uint32_t policy_rebuild_count;      /**< rivr_policy_build_program() successes          */
    uint32_t policy_reload_count;       /**< Engine hot-reloads triggered by policy change  */
    uint32_t duty_blocked_count;        /**< Reserved — set to 0 (use g_dc.blocked_count)  */
    uint32_t origination_drop_count;    /**< Reserved for future USB-origination gate       */
} rivr_policy_metrics_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise g_policy_params with RIVR_PARAM_* compiled-in defaults.
 * Call once from app_main() before rivr_embed_init().
 * Safe to call multiple times (idempotent).
 */
void rivr_policy_init(void);

/**
 * Validate and apply a single parameter update to g_policy_params.
 *
 * Bounds enforced (invalid values are silently ignored — no crash):
 *   beacon_interval_ms  >= 1000
 *   chat_throttle_ms    >= 100
 *   data_throttle_ms    >= 100
 *   duty_percent        in [1..10]
 */
void rivr_policy_set_param(uint8_t param_id, uint32_t value);

/**
 * Generate the current RIVR program string from g_policy_params into buf.
 * buf must be at least 512 bytes.  Always NUL-terminates.
 * Does not allocate heap memory.
 */
void rivr_policy_build_program(char *buf, size_t bufsz);

/**
 * Return a const pointer to the current policy parameters.
 * Safe to call anytime after rivr_policy_init().
 */
const rivr_policy_params_t *rivr_policy_params_get(void);

/**
 * Return a const pointer to the current policy metrics counters.
 * Safe to call anytime after rivr_policy_init().
 */
const rivr_policy_metrics_t *rivr_policy_metrics_get(void);

/**
 * Increment policy_reload_count.
 * Call from rivr_embed_reload() after a successful engine hot-reload.
 * Main-loop only — not ISR safe.
 */
void rivr_policy_notify_reload(void);

/**
 * Print current policy params + metrics as a single @POLICY JSON line to.
 * stdout (printf).  Use from CLI "policy" command or periodic diagnostics.
 * Format:
 *   @POLICY {"beacon":30000,"chat":2000,"data":2000,"duty":10,
 *            "updates":0,"last_update_ms":0,"rebuilds":0,"reloads":0,
 *            "duty_blocked":0,"orig_drops":0}\r\n
 */
void rivr_policy_print(void);

/**
 * @brief Self-test: validate counter behaviour with valid/invalid param sets.
 * Compiled in only when RIVR_POLICY_SELFTEST is defined.
 * Calls assert() on failure.
 */
#ifdef RIVR_POLICY_SELFTEST
void rivr_policy_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
