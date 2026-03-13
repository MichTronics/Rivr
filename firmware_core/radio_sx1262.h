/**
 * @file  radio_sx1262.h
 * @brief SX1262 LoRa radio driver interface.
 *
 * ARCHITECTURE RULES
 * ──────────────────
 *  • The DIO1 (packet_done) ISR is the ONLY code that writes to rf_rx_ringbuf.
 *  • The ISR NEVER calls into RIVR. It is 100% data-path only.
 *  • Transmit is driven by the main loop via the tx_queue (SPSC).
 *  • Every TX goes through the duty-cycle limiter before hitting the radio.
 *
 * FRAME FORMAT (RF_MAX_PAYLOAD_LEN bytes max):
 *   Byte 0     : frame type (0x01=DATA, 0x02=BEACON, 0x03=ACK, 0x04=CHAT)
 *   Bytes 1-2  : sender Lamport tick (little-endian uint16)
 *   Bytes 3+   : payload (UTF-8 text for CHAT, binary for DATA/ACK)
 */

#ifndef RADIO_SX1262_H
#define RADIO_SX1262_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"     /* IRAM_ATTR */
#include "ringbuf.h"
#include "timebase.h"

/* ── Configuration constants ─────────────────────────────────────────────── */

#define RF_MAX_PAYLOAD_LEN   255u   /**< SX1262 hardware limit                */
#define RF_RX_RINGBUF_CAP    8u     /**< Power-of-2; max 8 unread RX frames   */
#define RF_TX_QUEUE_CAP      4u     /**< Power-of-2; max 4 pending TX frames  */

/* LoRa parameters — all overrideable via variants/<device>/config.h or -D.
 * Set BEFORE including this header (e.g. via the -include force-include).
 * ─── Common values ───────────────────────────────────────────────────────
 *   SF: 7..12         higher = longer range, slower, more airtime
 *   BW: 125 kHz       standard EU868; 250/500 for higher throughput
 *   CR: 8 (= 4/8)     highest redundancy; 5 (= 4/5) for least overhead
 *   Power (SX1262): -9..22 dBm; E22 external PA adds ~8 dBm on top       */
#ifndef RF_SPREADING_FACTOR
#  define RF_SPREADING_FACTOR  8u     /**< SF7..SF12 */
#endif
#ifndef RF_BANDWIDTH_KHZ
#  define RF_BANDWIDTH_KHZ     125u   /**< 7/10/15/20/31/41/62/125/250/500 kHz */
#endif
#ifndef RF_CODING_RATE
#  define RF_CODING_RATE       8u     /**< denominator of 4/N — 5..8 */
#endif
#ifndef RF_PREAMBLE_LEN
#  define RF_PREAMBLE_LEN      8u
#endif
#ifndef RF_TX_POWER_DBM
#  define RF_TX_POWER_DBM      22     /**< SX1262: -9..22; SX1276: 2..20 dBm */
#endif
/* RF_FREQ_HZ — actual chip register value; falls back to RIVR_RF_FREQ_HZ so
 * a single -DRIVR_RF_FREQ_HZ=… updates both the boot banner AND the radio. */
#ifndef RF_FREQ_HZ
#  ifdef RIVR_RF_FREQ_HZ
#    define RF_FREQ_HZ  RIVR_RF_FREQ_HZ
#  else
#    define RF_FREQ_HZ  869480000UL   /**< 869.480 MHz (EU868 g3 high-power) */
#  endif
#endif

/**
 * Symbol period in microseconds for the configured SF and BW.
 *   T_sym_us = 2^SF * 1000 / BW_kHz
 * Uses RF_SPREADING_FACTOR and RF_BANDWIDTH_KHZ, both of which are set by
 * the variant platformio.ini (or default to SF=8, BW=125).  Note: BW values
 * like 62 (62.5 kHz) incur a <1% rounding error, which is acceptable.
 */
#define RF_TOA_T_SYM_US  ((1u << RF_SPREADING_FACTOR) * 1000u / RF_BANDWIDTH_KHZ)

/**
 * Time-on-Air in microseconds for a payload of `pl` bytes.
 * Parameterised by RF_SPREADING_FACTOR and RF_BANDWIDTH_KHZ so it stays
 * correct when either is changed (e.g. BW=62 kHz for the E22-900 variant).
 * Formula: SX1262 datasheet §6.1.4, CR=4/8, explicit header, CRC on, LDRO off.
 *
 *   T_sym      = 2^SF * 1000 / BW_kHz            (= RF_TOA_T_SYM_US)
 *   t_preamble = (N_pre + 4.25) × T_sym = 12.25 × T_sym  [49 * T_sym / 4]
 *   n_payload  = floor((8×PL + 43) / 32) × 8     [symbols, CR4/8]
 *   t_payload  = n_payload × T_sym
 *   ToA        = t_preamble + t_payload
 *
 * Verified sample values at SF8 / BW125 kHz:
 *   PL=15 → 107 ms  |  PL=20 → 123 ms  |  PL=30 → 156 ms  |  PL=50 → 238 ms
 * At SF8 / BW62 kHz all values are approximately 2× the BW125 figures.
 */
#define RF_TOA_APPROX_US(pl) \
    ((49u * (uint32_t)RF_TOA_T_SYM_US / 4u) + \
     (((8u * (uint32_t)(pl) + 43u) / 32u) * 8u * (uint32_t)RF_TOA_T_SYM_US))

/* ── Frame types ─────────────────────────────────────────────────────────── */
typedef enum {
    RF_FRAME_DATA   = 0x01,
    RF_FRAME_BEACON = 0x02,
    RF_FRAME_ACK    = 0x03,
    RF_FRAME_CHAT   = 0x04,
} rf_frame_type_t;

/* ── RX frame (pushed by ISR into rf_rx_ringbuf) ─────────────────────────── */
typedef struct {
    uint8_t  data[RF_MAX_PAYLOAD_LEN]; /**< Raw frame bytes                   */
    uint8_t  len;                      /**< Actual byte count                 */
    int16_t  rssi_dbm;                 /**< Signal strength                   */
    int8_t   snr_db;                   /**< Signal-to-noise ratio             */
    uint32_t rx_mono_ms;               /**< tb_millis() when DIO1 fired       */
    uint32_t from_id;                  /**< Immediate-neighbour node ID that
                                        *   delivered this frame (direct hop).
                                        *   0 = unknown (real HW, hop > 0).
                                        *   In simulation: set to relay's ID.
                                        *   Used by route_cache_learn_rx().   */
    uint8_t  iface;                    /**< Transport this frame arrived on.
                                        *   Uses rivr_iface_t values:
                                        *     0 = RIVR_IFACE_LORA  (default)
                                        *     1 = RIVR_IFACE_BLE
                                        *     2 = RIVR_IFACE_USB
                                        *   Defaults to 0 because the SX1262
                                        *   ISR does memset(&frame,0,…) before
                                        *   filling fields — LoRa frames need
                                        *   no explicit iface assignment.    */
} rf_rx_frame_t;

/* ── TX request (written by RIVR emit, consumed by main loop) ────────────── */
typedef struct {
    uint8_t  data[RF_MAX_PAYLOAD_LEN]; /**< Frame to transmit                 */
    uint8_t  len;                      /**< Byte count                        */
    uint32_t toa_us;                   /**< Estimated Time-on-Air (µs)        */
    uint32_t due_ms;                   /**< Earliest tb_millis() to transmit. *
                                        *   0 = send immediately.             *
                                        *   Set by routing_forward_delay_ms() *
                                        *   for jittered relay frames.        */
} rf_tx_request_t;

/* ── Global ring-buffers (defined in radio_sx1262.c) ─────────────────────── */
extern rb_t rf_rx_ringbuf;   /**< ISR → main loop (RX frames)   */
extern rb_t rf_tx_queue;     /**< RIVR emit → radio task (TX)  */

/* ── Radio lifecycle ─────────────────────────────────────────────────────── */

/** Initialise SPI, GPIO, and configure the SX1262 registers. */
void radio_init(void);

/**
 * @brief Initialise ONLY the ring-buffers without touching SPI or GPIO.
 *
 * Used in simulation mode (RIVR_SIM_MODE) where no hardware is present.
 * The global rf_rx_ringbuf and rf_tx_queue are ready to use after this call.
 *
 * [HARDWARE-TODO / SIM-ONLY] TODO(SX1262): remove when radio_init() is confirmed working on hardware.
 */
void radio_init_buffers_only(void);

/** Start continuous RX mode. Call after radio_init(). */
void radio_start_rx(void);

/**
 * @brief Perform a hard reset and full re-initialisation of the SX1262.
 *
 * Called automatically after 3 consecutive TX failures or 5 spurious DIO1
 * events.  Increments g_rivr_metrics.radio_hard_reset.  Safe to call from
 * the main loop only — never from an ISR or another task.
 */
void radio_hard_reset(void);

/**
 * @brief Transmit a single frame (blocking until TX_DONE or timeout).
 *
 * MUST only be called from the main loop (not from ISR).
 * MUST be called AFTER dutycycle_check() returns true.
 *
 * @return true  if TX completed (DIO1 TX_DONE within timeout)
 * @return false if timeout or SX1262 error
 */
bool radio_transmit(const rf_tx_request_t *req);

/**
 * @brief Non-blocking poll: if DIO1 fired, read the packet into the ringbuf.
 *
 * Called by main loop as a fallback when the ISR is not used (polling mode).
 * In ISR mode this function is a no-op.
 */
void radio_poll_rx(void);

/**
 * @brief Read the instantaneous RSSI from the SX1262 while in RX mode.
 *
 * Uses GetRssiInst (opcode 0x15).  Must be called from the main task only
 * (not from ISR).  Returns -120 dBm when the radio is not in RX mode
 * (e.g. during a TX window).
 *
 * @return Instantaneous RSSI in dBm (e.g. -87 for a fairly strong signal).
 */
int16_t radio_get_rssi_inst(void);

/* ── ISR (attached to DIO1 GPIO) ─────────────────────────────────────────── */

/**
 * @brief DIO1 interrupt handler.
 *
 * Sets an internal flag ONLY.  NO SPI calls are made here — spi_device_transmit
 * takes a FreeRTOS semaphore which is illegal from ISR context and would
 * trigger the Interrupt WDT.  All actual SPI work is deferred to
 * radio_service_rx() which must be called from the main-loop task.
 *
 * Attached with:  gpio_isr_handler_add(DIO1_PIN, radio_isr, NULL);
 */
void radio_isr(void *arg);  /* IRAM_ATTR on the definition in radio_sx1262.c */

/**
 * @brief Drain all pending DIO1 events (RxDone).
 *
 * Must be called from the main-loop task on every iteration (before
 * rivr_tick so received frames are in the ring-buffer for processing).
 * Does nothing if no DIO1 event is pending.
 */
void radio_service_rx(void);

/**
 * @brief Check for radio-silence and BUSY-stuck timeouts.
 *
 * Call once per main-loop iteration (after radio_service_rx()).
 * Triggers a guarded hard reset if either:
 *   • The radio has been in continuous-RX for >RADIO_RX_SILENCE_MS without
 *     a single DIO1 event (radio likely hung).
 * Backoff prevents more than one reset per RADIO_RESET_BACKOFF_MS.
 */
void radio_check_timeouts(void);

/* ── Fault injection (test builds only: -DRIVR_FAULT_INJECT=1) ──────────── */
#ifdef RIVR_FAULT_INJECT
/** Force platform_sx1262_wait_busy() to return false (simulate BUSY stuck). */
extern bool g_fault_busy_stuck;
/** Suppress TxDone IRQ flag inside TX polling loop (simulate TX never done). */
extern bool g_fault_tx_no_done;
/** Suppress DIO1 event dispatch (simulate RX silence). */
extern bool g_fault_rx_silence;
/** Simulate PayloadCrcError on next N received frames (burst CRC failures).
 *  Each call to radio_service_rx() that fires decrements this counter by 1,
 *  increments radio_rx_crc_fail, and discards the frame — exactly as the
 *  SX1262 hardware would if IRQ bit 0x0040 is set.  Set to 0 to disable. */
extern uint8_t g_fault_crc_fail;
/** Reset all internal radio statics — for test isolation only. */
void radio_fault_reset_state(void);
#endif /* RIVR_FAULT_INJECT */

/* ── Packet decoder (main-loop only) ────────────────────────────────────── */

/**
 * @brief Decode a raw RX frame into a RIVR-compatible FixedText<128> string.
 *
 * Output format: "CHAT:<text>" | "DATA:<hex>" | "BEACON:<tick>"
 * Returns actual string length (without null terminator).
 *
 * @param frame    raw rx frame
 * @param out_buf  destination buffer (at least 128 bytes)
 * @param out_len  buffer capacity
 * @return number of bytes written (0 on error)
 */
uint8_t radio_decode_frame(const rf_rx_frame_t *frame, char *out_buf, uint8_t out_len);

/**
 * @brief Extract sender Lamport tick from a frame header.
 * Returns 0 if the frame is too short.
 */
uint16_t radio_frame_sender_tick(const rf_rx_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_SX1262_H */
