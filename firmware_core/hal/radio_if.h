/**
 * @file  radio_if.h
 * @brief Abstract radio HAL interface for RIVR firmware.
 *
 * PURPOSE
 * ───────
 * Decouples higher-level firmware logic (main loop, routing, duty-cycle)
 * from the concrete radio driver (SX1262 / SX1276).  Concrete drivers
 * implement this interface; callers use only the abstract functions below.
 *
 * DESIGN RULES
 * ────────────
 * • Zero heap — the vtable pointer lives in BSS, populated once at init.
 * • ISR safety: only radio_if_push_rx_event() may be called from ISR context;
 *   all other functions MUST be called from the main-loop task.
 * • The concrete driver owns all hardware resources (SPI, GPIO, IRQ).
 *   The interface never reaches inside driver internals.
 *
 * USAGE
 * ─────
 *   // In main.c or platform init:
 *   radio_if_register(&g_sx1262_radio_if);   // or g_sx1276_radio_if
 *
 *   // Everywhere else:
 *   radio_if_transmit(buf, len, toa_us);
 *
 * CONCRETE IMPLEMENTATIONS
 * ────────────────────────
 *   • SX1262: radio_sx1262.c  — g_sx1262_radio_if
 *   • SX1276: radio_sx1276.c  — g_sx1276_radio_if
 *
 * NOTE: Existing code that calls radio_sx1262_* directly still works.
 * The vtable is an additive layer; no existing code needs to change.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── RX event (passed from ISR→main via ringbuf) ───────────────────────── */

/** Maximum over-the-air payload length (SX1262/SX1276 hardware limit). */
#define RADIO_IF_MAX_PAYLOAD 255u

/** One received radio frame, as filled by the driver ISR. */
typedef struct {
    uint8_t  buf[RADIO_IF_MAX_PAYLOAD]; /**< Raw received bytes              */
    uint8_t  len;                       /**< Valid bytes in buf              */
    int8_t   rssi_dbm;                  /**< Received signal strength        */
    int8_t   snr_db;                    /**< Signal-to-noise ratio           */
    uint32_t timestamp_ms;              /**< tb_millis() at RxDone interrupt  */
} radio_rx_event_t;

/* ── Abstract radio vtable ─────────────────────────────────────────────── */

/**
 * @brief Abstract radio driver interface.
 *
 * A concrete driver populates a static instance of this struct and registers
 * it via radio_if_register().  Callers then use radio_if_*() wrappers.
 */
typedef struct radio_if_vtable {
    /**
     * @brief Initialise radio hardware.
     * Called once from app_main() after SPI is up.
     * @return true on success, false on hardware fault.
     */
    bool (*init)(void);

    /**
     * @brief Enter continuous receive mode.
     * Safe to call multiple times (idempotent).
     */
    void (*start_rx)(void);

    /**
     * @brief Poll for pending ISR events and drain them from the ring buffer.
     * Appends decoded RX events to the provided array.
     *
     * @param out     Caller-supplied array to receive events.
     * @param max_out Maximum events to drain per call.
     * @return        Number of events written into @p out.
     */
    uint8_t (*service_rx)(radio_rx_event_t *out, uint8_t max_out);

    /**
     * @brief Transmit a frame.
     * Blocks until TxDone (or timeout).  Must NOT be called from ISR.
     *
     * @param buf    Frame bytes to transmit.
     * @param len    Frame length in bytes.
     * @param toa_us Expected time-on-air in µs (used for timeout guard).
     * @return       true on successful TxDone, false on timeout / HW error.
     */
    bool (*transmit)(const uint8_t *buf, uint8_t len, uint32_t toa_us);

    /**
     * @brief Query whether the radio is currently busy (BUSY pin high).
     * Used by duty-cycle logic to avoid back-to-back operations.
     */
    bool (*is_busy)(void);

    /**
     * @brief Perform a full hardware reset and re-initialise.
     * Called by the watchdog recovery path on repeated TX / BUSY failures.
     */
    void (*hard_reset)(void);

    /** Human-readable driver name, e.g. "SX1262" (for logging / build info). */
    const char *name;
} radio_if_vtable_t;

/* ── Global registration and dispatch ─────────────────────────────────── */

/**
 * @brief Register a concrete radio driver.
 * Must be called exactly once before any radio_if_*() function.
 *
 * @param vtable  Pointer to the driver's static vtable instance.
 */
void radio_if_register(const radio_if_vtable_t *vtable);

/**
 * @brief Returns the currently registered vtable (NULL if not yet registered).
 * Useful for diagnostic / logging purposes.
 */
const radio_if_vtable_t *radio_if_get(void);

/* ── Convenience wrappers (call through the registered vtable) ─────────── */

/** @see radio_if_vtable_t::init */
static inline bool radio_if_init(void)
{
    extern const radio_if_vtable_t *g_radio_if;
    return g_radio_if && g_radio_if->init ? g_radio_if->init() : false;
}

/** @see radio_if_vtable_t::start_rx */
static inline void radio_if_start_rx(void)
{
    extern const radio_if_vtable_t *g_radio_if;
    if (g_radio_if && g_radio_if->start_rx) g_radio_if->start_rx();
}

/** @see radio_if_vtable_t::service_rx */
static inline uint8_t radio_if_service_rx(radio_rx_event_t *out, uint8_t max_out)
{
    extern const radio_if_vtable_t *g_radio_if;
    return (g_radio_if && g_radio_if->service_rx)
        ? g_radio_if->service_rx(out, max_out) : 0;
}

/** @see radio_if_vtable_t::transmit */
static inline bool radio_if_transmit(const uint8_t *buf, uint8_t len, uint32_t toa_us)
{
    extern const radio_if_vtable_t *g_radio_if;
    return (g_radio_if && g_radio_if->transmit)
        ? g_radio_if->transmit(buf, len, toa_us) : false;
}

/** @see radio_if_vtable_t::is_busy */
static inline bool radio_if_is_busy(void)
{
    extern const radio_if_vtable_t *g_radio_if;
    return g_radio_if && g_radio_if->is_busy && g_radio_if->is_busy();
}

/** @see radio_if_vtable_t::hard_reset */
static inline void radio_if_hard_reset(void)
{
    extern const radio_if_vtable_t *g_radio_if;
    if (g_radio_if && g_radio_if->hard_reset) g_radio_if->hard_reset();
}

/** Returns the registered driver name or "none" when no driver registered. */
static inline const char *radio_if_name(void)
{
    extern const radio_if_vtable_t *g_radio_if;
    return (g_radio_if && g_radio_if->name) ? g_radio_if->name : "none";
}

#ifdef __cplusplus
}
#endif
