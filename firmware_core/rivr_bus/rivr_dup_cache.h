/**
 * @file  rivr_dup_cache.h
 * @brief Bus-level cross-transport duplicate frame cache.
 *
 * Prevents the same logical frame (identified by src_id + pkt_id) from being
 * processed twice when it arrives on more than one transport within a short
 * window.  This is distinct from routing.c's dedupe_cache_t, which prevents
 * relay loops — the bus cache fires earlier and skips the full routing
 * pipeline for iface-level duplicates.
 *
 * Example: phone sends a frame over BLE; a nearby node relays it back over
 * LoRa.  Without the bus cache both copies enter the routing engine; with it,
 * the second arrival is dropped cheaply before any decode work.
 *
 * Design constraints:
 *   • Zero heap: all storage is BSS.
 *   • 64 entries × 8 bytes = 512 bytes BSS total.
 *   • O(N) linear scan — ~10 µs worst-case on Xtensa LX6 @ 240 MHz.
 *   • Ring eviction (head advances mod RIVR_BUS_DUP_CACHE_SIZE).
 *   • Not ISR-safe; called only from the main-loop task.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/** Number of (src_id, pkt_id) pairs tracked simultaneously.
 *  Power-of-two so modulo is a single AND. */
#define RIVR_BUS_DUP_CACHE_SIZE  64u

/** Single entry: identifies a unique logical packet in the mesh. */
typedef struct {
    uint32_t src_id;   /**< Originator node ID (0 = invalid / empty slot) */
    uint16_t pkt_id;   /**< Per-source packet identifier                   */
    uint8_t  _pad[2];  /**< Explicit padding; keeps struct at 8 bytes      */
} rivr_bus_dup_entry_t;

/** The complete duplicate cache, zero-initialised in BSS. */
typedef struct {
    rivr_bus_dup_entry_t entries[RIVR_BUS_DUP_CACHE_SIZE];
    uint8_t head;   /**< Next slot to overwrite (ring eviction pointer) */
} rivr_bus_dup_cache_t;

/**
 * @brief Test whether (src_id, pkt_id) is already present in the cache.
 * @return true  if the pair is present (frame is a cross-transport duplicate)
 * @return false if the pair is new (first time seen on any transport)
 */
bool rivr_bus_dup_cache_seen(const rivr_bus_dup_cache_t *cache,
                              uint32_t src_id, uint16_t pkt_id);

/**
 * @brief Insert (src_id, pkt_id) into the cache.
 *
 * Overwrites the slot at cache->head and advances head mod SIZE.
 * Call this after rivr_bus_dup_cache_seen() returns false to record the
 * new frame.
 */
void rivr_bus_dup_cache_add(rivr_bus_dup_cache_t *cache,
                             uint32_t src_id, uint16_t pkt_id);
