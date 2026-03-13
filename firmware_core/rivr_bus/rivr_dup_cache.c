/**
 * @file  rivr_dup_cache.c
 * @brief Bus-level cross-transport duplicate frame cache implementation.
 */

#include "rivr_dup_cache.h"

bool rivr_bus_dup_cache_seen(const rivr_bus_dup_cache_t *cache,
                              uint32_t src_id, uint16_t pkt_id)
{
    /* src_id == 0 is never a real node, so treat it as "not seen" */
    if (src_id == 0u) {
        return false;
    }

    for (uint8_t i = 0u; i < RIVR_BUS_DUP_CACHE_SIZE; i++) {
        if (cache->entries[i].src_id == src_id &&
            cache->entries[i].pkt_id == pkt_id) {
            return true;
        }
    }
    return false;
}

void rivr_bus_dup_cache_add(rivr_bus_dup_cache_t *cache,
                             uint32_t src_id, uint16_t pkt_id)
{
    uint8_t slot = cache->head & (RIVR_BUS_DUP_CACHE_SIZE - 1u);
    cache->entries[slot].src_id = src_id;
    cache->entries[slot].pkt_id = pkt_id;
    cache->head = (uint8_t)((slot + 1u) & (RIVR_BUS_DUP_CACHE_SIZE - 1u));
}
