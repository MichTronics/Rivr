/**
 * @file  rivr_bus.h
 * @brief Rivr multi-transport packet bus — public API.
 *
 * The bus is the single point through which all received frames are
 * registered and fanned out to their eligible egress transports.
 *
 * Ingress flow (called from sources_rf_rx_drain() for every valid frame):
 *
 *   app_main loop
 *     └─ rivr_tick()
 *          └─ sources_rf_rx_drain()
 *               ├─ protocol_decode()              (caller validates)
 *               ├─ rivr_bus_receive()  ← BUS ENTRY POINT
 *               │    ├─ bus dup cache lookup
 *               │    ├─ per-iface RX counter increment
 *               │    └─ rivr_bus_dispatch()  → BLE/USB mirror
 *               └─ routing_flood_forward()         (relay / RIVR engine)
 *
 * Threading: all bus functions run on the main-loop task only.  Not
 * ISR-safe.  No locking required because SPSC ring-buffers isolate ISR
 * from main loop and BLE callbacks push to rf_rx_ringbuf under their own
 * synchronisation.
 */
#pragma once
#include "rivr_bus_types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialise the packet bus.
 *
 * Zeroes the cross-transport duplicate cache and logs the boot banner.
 * Call once from app_main() before the main loop starts.
 */
void rivr_bus_init(void);

/**
 * @brief Register a received frame with the bus and fan it out to egress
 *        transports.
 *
 * The caller (sources_rf_rx_drain) is responsible for calling
 * protocol_decode() first; this function performs a second lightweight
 * header decode to extract (src_id, pkt_id) for duplicate detection.
 *
 * Processing pipeline:
 *  1. Basic length bounds check (RIVR_PKT_MIN_FRAME ≤ len ≤ RF_MAX_PAYLOAD_LEN)
 *  2. Header decode to read (src_id, pkt_id)
 *  3. Bus-level cross-transport duplicate cache lookup
 *  4. Per-interface RX counter increment
 *  5. Compute egress mask via rivr_dispatch_select()
 *  6. Fan out to eligible transports via rivr_bus_dispatch()
 *
 * @param data  Encoded Rivr frame bytes (already validated by caller)
 * @param len   Frame length in bytes
 * @param meta  Ingress metadata (iface, rssi, snr, timestamp)
 * @return true  Continue processing through routing pipeline
 * @return false Frame rejected at bus level (cross-transport dup or invalid)
 */
bool rivr_bus_receive(const uint8_t *data, size_t len,
                      const rivr_rx_meta_t *meta);

/**
 * @brief Inject a locally-generated frame into all active transports.
 *
 * Equivalent to calling rivr_bus_receive() with source=RIVR_IFACE_LOCAL.
 * Used by program-layer code that wants to originate a frame directly into
 * the multi-transport pipeline without going through the RIVR engine.
 *
 * @param data  Encoded Rivr frame bytes
 * @param len   Frame length in bytes
 * @return true  if the frame was accepted and dispatched to at least one transport
 */
bool rivr_bus_send_local(const uint8_t *data, size_t len);

/**
 * @brief Fan out a frame to a set of transport interfaces.
 *
 * Iterates the target bitmask and calls the corresponding transport send
 * function for each set bit, skipping the source transport if its bit
 * appears in the mask.  Updates bus_forward_* metrics.
 *
 * @param frame        Encoded Rivr frame bytes
 * @param len          Frame length in bytes
 * @param targets      Bitmask of RIVR_MASK_* values
 * @param source_iface Transport to exclude (never echo back)
 * @return true  if at least one transport accepted the frame
 */
bool rivr_bus_dispatch(const uint8_t *frame, size_t len,
                       rivr_iface_mask_t targets, rivr_iface_t source_iface);
