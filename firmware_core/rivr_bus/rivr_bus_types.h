/**
 * @file  rivr_bus_types.h
 * @brief Shared type definitions for the Rivr multi-transport packet bus.
 *
 * This header is the single source of truth for transport identifiers and
 * bitmasks.  Include it anywhere you need to reference a transport iface.
 * It has no other firmware_core dependencies (only stdint.h), so it is safe
 * to include from ISR-context headers.
 */
#pragma once
#include <stdint.h>

/* ── Transport interface identifiers ─────────────────────────────────────── */

/**
 * @brief Transport interface identifier.
 *
 * Values 0-4 are stable.  New physical transports must be appended BEFORE
 * RIVR_IFACE_COUNT and their mask constant added below.
 *
 * RIVR_IFACE_LORA = 0 is intentional: the ISR does memset(&frame,0,…) before
 * filling rf_rx_frame_t, so frames without an explicit iface= assignment
 * default to LoRa with no code change.
 */
typedef enum {
    RIVR_IFACE_LORA  = 0,  /**< SX1262 / SX1276 LoRa radio                   */
    RIVR_IFACE_BLE   = 1,  /**< NimBLE GATT characteristic write              */
    RIVR_IFACE_USB   = 2,  /**< USB-UART SLIP bridge (RIVR_FEATURE_USB_BRIDGE)*/
    RIVR_IFACE_WIFI  = 3,  /**< Future: 802.11 AP/STA bridge                  */
    RIVR_IFACE_LOCAL = 4,  /**< Frame originated by this node's program layer */
    RIVR_IFACE_COUNT = 5   /**< Sentinel — keep last                           */
} rivr_iface_t;

/* ── Per-frame ingress metadata ──────────────────────────────────────────── */

/**
 * @brief Metadata carried alongside each received frame through the bus.
 *
 * Populated by the transport driver before calling rivr_bus_receive().
 * rssi and snr are int8_t (bus-side); the radio layer's int16_t rssi_dbm
 * is cast at the call site in sources_rf_rx_drain().
 */
typedef struct {
    rivr_iface_t iface;        /**< Transport the frame arrived on             */
    int8_t       rssi;         /**< Received signal strength in dBm (≈ –120…0)*/
    int8_t       snr;          /**< Signal-to-noise ratio in dB                */
    uint32_t     timestamp_ms; /**< Monotonic receive timestamp (tb_millis())  */
    uint8_t      flags;        /**< Reserved for future use; set to 0          */
} rivr_rx_meta_t;

/* ── Transport bitmask type ──────────────────────────────────────────────── */

/** Bitmask selecting one or more transport interfaces for dispatch. */
typedef uint32_t rivr_iface_mask_t;

#define RIVR_MASK_NONE   ((rivr_iface_mask_t)0u)
#define RIVR_MASK_LORA   ((rivr_iface_mask_t)(1u << RIVR_IFACE_LORA))   /* 0x01 */
#define RIVR_MASK_BLE    ((rivr_iface_mask_t)(1u << RIVR_IFACE_BLE))    /* 0x02 */
#define RIVR_MASK_USB    ((rivr_iface_mask_t)(1u << RIVR_IFACE_USB))    /* 0x04 */
#define RIVR_MASK_WIFI   ((rivr_iface_mask_t)(1u << RIVR_IFACE_WIFI))   /* 0x08 */
#define RIVR_MASK_LOCAL  ((rivr_iface_mask_t)(1u << RIVR_IFACE_LOCAL))  /* 0x10 */

/** LoRa + future WiFi = all long-range RF transports */
#define RIVR_MASK_ALL_RF  (RIVR_MASK_LORA | RIVR_MASK_WIFI)
/** All transports (excludes LOCAL which is not a real iface) */
#define RIVR_MASK_ALL     (RIVR_MASK_LORA | RIVR_MASK_BLE | RIVR_MASK_USB | RIVR_MASK_WIFI)
