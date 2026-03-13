/**
 * @file  rivr_dispatch.h
 * @brief Egress fanout policy for the Rivr multi-transport packet bus.
 *
 * rivr_dispatch_select() answers: "given a frame that arrived on iface X,
 * which other transports should receive a copy?"
 *
 * Policy summary:
 *  ┌──────────┬──────────────────────────────────────────────────────────┐
 *  │ Source   │ Eligible egress transports                               │
 *  ├──────────┼──────────────────────────────────────────────────────────┤
 *  │ LORA     │ BLE + USB — app-relevant types only                      │
 *  │ BLE      │ USB — app-relevant types only                            │
 *  │          │ (LoRa relay is handled by routing_flood_forward, not bus)│
 *  │ USB      │ BLE — app-relevant types only                            │
 *  │          │ (LoRa relay is handled by routing_flood_forward, not bus)│
 *  │ WIFI     │ BLE + USB — app-relevant types only                      │
 *  │ LOCAL    │ LORA + BLE + USB — all packet types                      │
 *  └──────────┴──────────────────────────────────────────────────────────┘
 *
 * "App-relevant" packets are those a host client (phone app, USB tool)
 * understands: CHAT, BEACON, DATA, TELEMETRY, MAILBOX, ALERT.
 * Routing control packets (ROUTE_REQ/RPL, ACK, PROG_PUSH) are not
 * mirrored — they are mesh-internal and would confuse host clients.
 *
 * IMPORTANT: LoRa relay for BLE/USB-sourced frames is handled by
 * routing_flood_forward() inside sources_rf_rx_drain(), NOT by the bus
 * dispatch.  Adding RIVR_MASK_LORA for BLE/USB sources would cause a
 * double-push to rf_tx_queue.
 */
#pragma once
#include "rivr_bus_types.h"
#include "firmware_core/protocol.h"

/**
 * @brief Test whether a packet type is relevant to host clients.
 *
 * App-relevant types are mirrored to BLE/USB so connected clients see
 * mesh traffic in real time.  Routing control types are mesh-internal.
 *
 * @param pkt_type  PKT_* constant from protocol.h
 * @return true  if the packet should be mirrored to client transports
 */
bool rivr_dispatch_is_app_relevant(uint8_t pkt_type);

/**
 * @brief Compute egress transport mask for a received frame.
 *
 * Returns the bitmask of transports that should receive a copy of this
 * frame.  The source transport is never included in the returned mask.
 *
 * @param pkt_type  PKT_* type byte from the decoded packet header
 * @param source    Transport the frame arrived on
 * @return Bitmask of zero or more RIVR_MASK_* values
 */
rivr_iface_mask_t rivr_dispatch_select(uint8_t pkt_type, rivr_iface_t source);
