/**
 * @file  radio_sx1276.h
 * @brief SX1276/SX1278 LoRa radio driver interface.
 *
 * Thin wrapper around radio_sx1262.h — includes all shared types, constants,
 * and function declarations (radio_init, radio_transmit, etc.) that are
 * identical in signature for both drivers.
 *
 * This header is included by radio_sx1276.c.  All other firmware files can
 * continue to include radio_sx1262.h; both files expose the same API so the
 * linker resolves the symbols to whichever driver is compiled in.
 *
 * SX1276-SPECIFIC PIN DEFAULTS (LilyGo LoRa32 V2.1_1.6)
 * ───────────────────────────────────────────────────────
 *  DIO0 = GPIO 26  (TxDone / RxDone — mapped to PIN_SX1262_DIO1 slot)
 *  DIO1 = GPIO 33  (RxTimeout — optional, not connected to ISR by default)
 *
 *  All other LoRa-SPI pins are configured via the variant config.h  and
 *  the shared PIN_SX1262_* namespace defined in platform_esp32.h.
 */

#ifndef RADIO_SX1276_H
#define RADIO_SX1276_H

/* Pull in all shared types (rf_rx_frame_t, rf_tx_request_t, rb_t,
 * RF_MAX_PAYLOAD_LEN, RF_SPREADING_FACTOR, RF_FREQ_HZ, RF_TOA_APPROX_US, …) */
#include "radio_sx1262.h"

/* ── SX1276 interrupt pin default ────────────────────────────────────────── */
/* PIN_SX1262_DIO1 is reused as the interrupt pin slot.  The LilyGo variant  */
/* config.h defines PIN_SX1262_DIO1=26 (= DIO0 on the SX1276 hardware),     */
/* so  gpio_isr_handler_add(PIN_SX1262_DIO1, radio_isr, NULL)  attaches to  */
/* the correct GPIO without changing any call site.                           */

/* Secondary DIO (RxTimeout on SX1276) — informational, not used by driver  */
#ifndef PIN_SX1276_DIO1
#  define PIN_SX1276_DIO1  33
#endif

#endif /* RADIO_SX1276_H */
