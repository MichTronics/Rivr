/**
 * @file  rivr_iface_usb.h
 * @brief USB-UART SLIP bridge transport adapter for the Rivr packet bus.
 *
 * Provides a byte-stream SLIP-framed (RFC 1055) bridge over a hardware UART,
 * typically wired to a USB-to-serial converter.  Enables a host computer or
 * Android device to act as a transparent RIVR mesh node.
 *
 * Enable at compile time with -DRIVR_FEATURE_USB_BRIDGE=1 (default: 0).
 * When disabled all functions compile to inline no-ops and the UART driver
 * is never installed, so there is zero overhead on nodes that do not need it.
 *
 * Hardware defaults (override at compile time):
 *   RIVR_USB_UART_NUM  = 1        (UART1; UART0 is the log/CLI port)
 *   RIVR_USB_UART_TX   = 17       (GPIO17)
 *   RIVR_USB_UART_RX   = 16       (GPIO16)
 *   RIVR_USB_UART_BAUD = 115200
 *
 * SLIP framing constants (RFC 1055):
 *   END     = 0xC0  — frame boundary
 *   ESC     = 0xDB  — escape byte
 *   ESC_END = 0xDC  — escaped END inside payload
 *   ESC_ESC = 0xDD  — escaped ESC inside payload
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef RIVR_FEATURE_USB_BRIDGE
#  define RIVR_FEATURE_USB_BRIDGE 0
#endif

#ifndef RIVR_USB_UART_NUM
#  define RIVR_USB_UART_NUM  1
#endif
#ifndef RIVR_USB_UART_TX
#  define RIVR_USB_UART_TX   17
#endif
#ifndef RIVR_USB_UART_RX
#  define RIVR_USB_UART_RX   16
#endif
#ifndef RIVR_USB_UART_BAUD
#  define RIVR_USB_UART_BAUD 115200
#endif

#if RIVR_FEATURE_USB_BRIDGE

/**
 * @brief Install the UART driver and configure GPIO pins.
 *
 * Must be called once from app_main() after platform_init().
 * Sets the internal s_usb_ready flag on success.
 */
void rivr_iface_usb_init(void);

/**
 * @brief Test whether the USB-UART driver was initialised successfully.
 * @return true if rivr_iface_usb_init() completed without error
 */
bool rivr_iface_usb_ready(void);

/**
 * @brief SLIP-encode and transmit a frame over the USB-UART.
 *
 * Non-blocking: writes to the UART TX FIFO / DMA buffer.  Returns false
 * immediately if the driver is not ready or uart_write_bytes fails.
 *
 * @param data  Encoded Rivr frame bytes
 * @param len   Frame length (1–255 bytes)
 * @return true  if all bytes were accepted by the UART driver
 * @return false on error
 */
bool rivr_iface_usb_send(const uint8_t *data, size_t len);

/**
 * @brief Drain UART RX FIFO, SLIP-decode, and push complete frames to
 *        rf_rx_ringbuf with iface=RIVR_IFACE_USB.
 *
 * Call once per main-loop iteration.  Reads up to 256 bytes per call to
 * bound worst-case time.  Complete frames are pushed directly into the
 * shared ring-buffer consumed by sources_rf_rx_drain().
 */
void rivr_iface_usb_tick(void);

#else  /* !RIVR_FEATURE_USB_BRIDGE — compile out completely */

static inline void rivr_iface_usb_init(void)                             {}
static inline bool rivr_iface_usb_ready(void)                      { return false; }
static inline bool rivr_iface_usb_send(const uint8_t *d, size_t l) { (void)d; (void)l; return false; }
static inline void rivr_iface_usb_tick(void)                             {}

#endif /* RIVR_FEATURE_USB_BRIDGE */
