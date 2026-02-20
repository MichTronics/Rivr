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

/* LoRa parameters (SF9 BW125kHz – good range/speed tradeoff for LoRaWAN EU) */
#define RF_SPREADING_FACTOR  9u
#define RF_BANDWIDTH_KHZ     125u
#define RF_CODING_RATE       5u     /**< 4/5 */
#define RF_PREAMBLE_LEN      8u
#define RF_FREQ_HZ           868100000UL  /**< 868.1 MHz (EU868 channel 0) */

/**
 * Approximate Time-on-Air in microseconds for a payload of `payload_bytes`
 * at SF9 BW125kHz CR4/5, header enabled, CRC enabled.
 * Formula: see SX1262 datasheet Section 6.1.4
 * At SF9, BW=125kHz: T_sym = 2^9 / 125000 ≈ 4.096 ms
 * n_payload = ceil((8*PL - 4*SF + 28 + 16 - 20*IH) / (4*(SF-2*DE))) * (CR+4)
 * For PL=50 bytes, SF9, BW125, no low-dr: ToA ≈ 370 ms
 */
#define RF_TOA_APPROX_US(payload_bytes) \
    ((uint32_t)(4096u + 4096u * (((payload_bytes) * 8u + 28u + 32u) / (4u * (RF_SPREADING_FACTOR - 2u)))))

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

/* ── ISR (attached to DIO1 GPIO) ─────────────────────────────────────────── */

/**
 * @brief DIO1 interrupt handler.
 *
 * Reads status register, fetches payload via SPI, pushes into rf_rx_ringbuf.
 * NEVER calls RIVR. NEVER allocates. Fully deterministic bounded-time path.
 *
 * Attached with:  gpio_isr_handler_add(DIO1_PIN, radio_isr, NULL);
 */
void IRAM_ATTR radio_isr(void *arg);

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
