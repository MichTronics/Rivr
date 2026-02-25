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

/* LoRa parameters (SF8 BW125kHz CR4/8 – 869.480 MHz EU868 high-power sub-band g3) */
#define RF_SPREADING_FACTOR  8u
#define RF_BANDWIDTH_KHZ     125u
#define RF_CODING_RATE       8u     /**< 4/8 */
#define RF_PREAMBLE_LEN      8u
#define RF_FREQ_HZ           869480000UL  /**< 869.480 MHz (EU868 g3 high-power) */

/**
 * Time-on-Air in microseconds for a payload of `pl` bytes.
 * SF8, BW=125kHz, CR=4/8, explicit header, CRC on, LDRO off.
 * Formula: SX1262 datasheet §6.1.4
 *
 *   T_sym      = 2^SF / BW = 2^8 / 125000 = 2048 µs
 *   t_preamble = (N_pre + 4.25) × T_sym = 12.25 × 2048 = 25088 µs
 *   n_payload  = ceil((8×PL + 12) / 32) × 8  [symbols, SF8 CR4/8 IH=0]
 *   t_payload  = n_payload × T_sym
 *   ToA        = t_preamble + t_payload
 *
 * Verified sample values:
 *   PL=15 → 107 ms  |  PL=20 → 123 ms  |  PL=30 → 156 ms  |  PL=50 → 238 ms
 */
#define RF_TOA_APPROX_US(pl) \
    (25088u + (((8u * (uint32_t)(pl) + 43u) / 32u) * 16384u))

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
 * TODO(SX1262): remove when radio_init() is confirmed working on hardware.
 */
void radio_init_buffers_only(void);

/** Start continuous RX mode. Call after radio_init(). */
void radio_start_rx(void);

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
 * Reads status register, fetches payload via SPI, pushes into rf_rx_ringbuf.
 * NEVER calls RIVR. NEVER allocates. Fully deterministic bounded-time path.
 *
 * Attached with:  gpio_isr_handler_add(DIO1_PIN, radio_isr, NULL);
 */
void radio_isr(void *arg);  /* IRAM_ATTR on the definition in radio_sx1262.c */

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
