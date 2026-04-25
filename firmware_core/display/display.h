/**
 * @file  display/display.h
 * @brief Non-blocking SSD1306 128×64 I2C OLED status display.
 *
 * DESIGN GOALS
 * ────────────
 *  • Non-blocking: display_update() returns in < 1 ms when the 200 ms
 *    inter-update guard has not elapsed.  The I2C flush itself (≈ 23 ms at
 *    400 kHz, single 1025-byte burst in horizontal addressing mode) only
 *    happens once every DISPLAY_REFRESH_MS.
 *  • No dynamic memory: entire framebuffer and I2C handle are BSS-static.
 *  • No delays: timing driven by tb_millis() comparison.
 *  • No radio-timing impact: ISR pushes to ring-buffer regardless of whether
 *    the main task is blocked on I2C; the main loop catches up after flush.
 *
 * PAGES (auto-rotate every DISPLAY_PAGE_ROTATE_MS)
 * ──────────────────────────────────────────────────
 *  1 – Overview    (NodeID, NetID, Callsign, Uptime)
 *  2 – RF Stats    (RSSI, SNR, RX count, TX count)
 *  3 – Routing     (Neighbours, Routes, Pending queue)
 *  4 – Duty Cycle  (Used %, Remaining %, Backoff ms)
 *  5 – RIVR VM     (Cycles, Last event, Error code)
 *
 * FEATURE GATE
 * ────────────
 *  Compile with -DFEATURE_DISPLAY=1 to enable the real driver.
 *  Without it (default =0) every function is an inline no-op, so the
 *  display module compiles to zero code.
 *
 * INTEGRATION (main.c)
 * ─────────────────────
 *  display_stats_t disp = { ... };         // fill from globals
 *  display_init(&disp);                    // once, after platform_init()
 *
 *  for (;;) {
 *      rivr_tick();
 *      tx_drain_loop();
 *      display_fill_stats(&disp);          // cheap read of shared globals
 *      display_update(&disp);              // NOP until 200 ms elapsed
 *      vTaskDelay(pdMS_TO_TICKS(1));
 *  }
 */

#ifndef RIVR_DISPLAY_H
#define RIVR_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ── Feature gate ────────────────────────────────────────────────────────── */

#ifndef FEATURE_DISPLAY
#  define FEATURE_DISPLAY 0
#endif

/* ── Timing constants ────────────────────────────────────────────────────── */

/** Minimum interval between full display refreshes (200 ms → max 5 Hz). */
#define DISPLAY_REFRESH_MS      200u

/** Auto-rotate to the next page after this many milliseconds. */
#define DISPLAY_PAGE_ROTATE_MS  3000u

/* ── Optional-page feature guards ──────────────────────────────────────────
 *
 * These are normally set by platformio build flags (-DRIVR_FABRIC_REPEATER=1
 * etc.).  The #ifndef defaults ensure display.h is safe to include from any
 * translation unit that does not set them.
 * ─────────────────────────────────────────────────────────────────────────── */
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER 0
#endif
#ifndef RIVR_FEATURE_DS18B20
#  define RIVR_FEATURE_DS18B20 0
#endif
#ifndef RIVR_FEATURE_AM2302
#  define RIVR_FEATURE_AM2302  0
#endif
#ifndef RIVR_FEATURE_VBAT
#  define RIVR_FEATURE_VBAT    0
#endif

#if RIVR_FEATURE_DS18B20 || RIVR_FEATURE_AM2302 || RIVR_FEATURE_VBAT
#  define DISPLAY_HAS_SENSORS  1
#  define DISPLAY_SENSOR_PAGES 1u
#else
#  define DISPLAY_HAS_SENSORS  0
#  define DISPLAY_SENSOR_PAGES 0u
#endif

#if RIVR_FABRIC_REPEATER
#  define DISPLAY_HAS_FABRIC   1
#  define DISPLAY_FABRIC_PAGES 1u
#else
#  define DISPLAY_HAS_FABRIC   0
#  define DISPLAY_FABRIC_PAGES 0u
#endif

/** Total pages: 6 base + GPS page + optional sensor page + optional fabric page. */
#define DISPLAY_PAGES         (7u + DISPLAY_SENSOR_PAGES + DISPLAY_FABRIC_PAGES)

/** Index of the GPS / position page (always present). */
#define DISP_PAGE_GPS         6u
/** Index of the sensor page (only valid when DISPLAY_HAS_SENSORS). */
#define DISP_PAGE_SENSORS     7u
/** Index of the fabric page (only valid when DISPLAY_HAS_FABRIC). */
#define DISP_PAGE_FABRIC      (7u + DISPLAY_SENSOR_PAGES)

/** Number of neighbour slots carried in the stats snapshot for page 5. */
#define DISPLAY_NEIGHBOR_MAX    3u

/* ── I2C pin defaults (can be overridden by compiler flags) ─────────────── */

#ifndef PIN_DISPLAY_SDA
#  define PIN_DISPLAY_SDA  21
#endif
#ifndef PIN_DISPLAY_SCL
#  define PIN_DISPLAY_SCL  22
#endif

/** SSD1306 7-bit I2C address (OLED SA0 pin low = 0x3C, high = 0x3D). */
#ifndef DISPLAY_I2C_ADDR
#  define DISPLAY_I2C_ADDR  0x3Cu
#endif

/* ── Stats structure ─────────────────────────────────────────────────────── *
 *
 * Caller (main.c) fills this struct each loop iteration and passes it to
 * display_update().  The display module never reads global state directly,
 * keeping it free of cross-module dependencies.
 * ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Page 0 – Overview */
    uint32_t node_id;       /**< Full 32-bit node ID                         */
    uint16_t net_id;        /**< Network / channel discriminator             */
    char     callsign[12];  /**< Amateur callsign or node name (NUL-terminated) */
    uint32_t uptime_s;      /**< Seconds since boot (tb_millis() / 1000)    */
    bool     ble_active;    /**< BLE advertising / transport active         */
    bool     ble_connected; /**< BLE client connected                       */
    bool     ble_paired;    /**< At least one persisted BLE bond exists     */
    uint32_t ble_passkey;   /**< Active 6-digit BLE passkey; 0 when open    */

    /* Page 1 – RF Stats */
    int16_t  rssi_inst_dbm; /**< Instantaneous RSSI polled from SX1262 (dBm) */
    int16_t  rssi_dbm;      /**< RSSI of last received frame (dBm)          */
    int8_t   snr_db;        /**< SNR of last received frame (dB)            */
    uint32_t rx_count;      /**< Total RX frames decoded since boot         */
    uint32_t tx_count;      /**< Total TX frames queued since boot          */

    /* Page 2 – Routing */
    uint8_t  neighbor_count; /**< Active entries in neighbour table          */
    uint8_t  route_count;    /**< Active entries in route cache              */
    uint8_t  pending_count;  /**< Pending messages awaiting route resolution */

    /* Page 3 – Duty Cycle */
    uint16_t dc_used_pct_x10;  /**< Duty cycle used × 10 (e.g. 23 = 2.3 %) */
    uint32_t dc_backoff_ms;    /**< Milliseconds until next TX window opens  */

    /* Page 4 – RIVR VM */
    uint32_t vm_cycles;        /**< Accumulated VM scheduler cycles          */
    char     last_event[16];   /**< Name of most recent RIVR event (or "")  */
    uint32_t error_code;       /**< Last non-OK RIVR error code (0 = none)  */

    /* Page 5 – Neighbours */
    struct {
        char     callsign[12]; /**< Callsign from last beacon, or node ID    */
        int16_t  rssi_dbm;     /**< RSSI of last received frame from node    */
        uint32_t age_s;        /**< Seconds since last heard                 */
        uint8_t  hop_count;    /**< Minimum hop distance                     */
        bool     valid;        /**< true when slot contains live data         */
    } neighbors[DISPLAY_NEIGHBOR_MAX];

    /* Optional page – Sensors (all zero when DISPLAY_HAS_SENSORS=0) */
    bool    sensor_ds18b20_valid;       /**< true when a DS18B20 reading is available   */
    int32_t sensor_ds18b20_temp_x100;   /**< DS18B20 temperature (°C × 100)             */
    bool    sensor_am2302_rh_valid;     /**< true when an AM2302 RH reading is available */
    int32_t sensor_am2302_rh_x100;     /**< AM2302 relative humidity (% RH × 100)      */
    bool    sensor_am2302_temp_valid;   /**< true when an AM2302 temp reading is available */
    int32_t sensor_am2302_temp_x100;   /**< AM2302 temperature (°C × 100)               */
    bool    sensor_vbat_valid;          /**< true when a VBAT reading is available       */
    int32_t sensor_vbat_mv;            /**< Battery voltage (mV)                        */

    /* GPS / Position page (loaded from NVS or live GPS; INT32_MIN = not set) */
    int32_t  gps_lat_e7;    /**< Latitude × 1e7; INT32_MIN = not set          */
    int32_t  gps_lon_e7;    /**< Longitude × 1e7; INT32_MIN = not set         */
    bool     gps_from_nvs;  /**< true = sourced from NVS, false = live GPS    */

    /* Optional page – Fabric debug (from rivr_fabric_get_debug; zero when RIVR_FABRIC_REPEATER=0) */
    uint8_t  fabric_score;              /**< Congestion score 0..100                    */
    uint16_t fabric_rx_per_s_x100;      /**< RX rate × 100  (e.g. 150 = 1.50/s)         */
    uint16_t fabric_blocked_per_s_x100; /**< DC-blocked rate × 100                      */
    uint16_t fabric_fail_per_s_x100;    /**< TX-fail rate × 100                         */
    uint32_t fabric_relay_drop;         /**< Lifetime relay DROP count                  */
    uint32_t fabric_relay_delay;        /**< Lifetime relay DELAY count                 */
} display_stats_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

#if FEATURE_DISPLAY

/**
 * @brief Spawn the display FreeRTOS task.
 *
 * Creates a low-priority task (priority 1, 4 kB stack) that handles all
 * I2C communication.  Must be called once from app_main() after
 * platform_init().  Returns immediately — never blocks the caller.
 *
 * @param initial  Initial stats shown on the boot screen.  Copied
 *                 internally; pointer need not remain valid after return.
 *                 May be NULL (boot screen shows placeholder "---").
 */
void display_task_start(const display_stats_t *initial);

/**
 * @brief Publish a stats snapshot for the display task to consume.
 *
 * Copies @p stats into an internal buffer that the display task reads
 * at its next refresh.  Returns immediately; never blocks.
 * Safe to call from the main loop every iteration.
 *
 * @param stats  Pointer to current statistics; must not be NULL.
 */
void display_post_stats(const display_stats_t *stats);

#else /* FEATURE_DISPLAY == 0 — compile to empty stubs */

static inline void display_task_start(const display_stats_t *s) { (void)s; }
static inline void display_post_stats(const display_stats_t *s) { (void)s; }

#endif /* FEATURE_DISPLAY */

#ifdef __cplusplus
}
#endif

#endif /* RIVR_DISPLAY_H */
