#include "rivr_ble_frag.h"

#include <string.h>

void rivr_ble_frag_reset(rivr_ble_frag_rx_t *rx)
{
    uint8_t i;

    if (!rx) {
        return;
    }

    rx->next_slot = 0u;
    for (i = 0u; i < RIVR_BLE_FRAG_RX_SLOTS; ++i) {
        rx->slots[i].active = false;
        rx->slots[i].msg_id = 0u;
        rx->slots[i].total_len = 0u;
        rx->slots[i].received_len = 0u;
    }
}

static bool rivr_ble_frag_is_packet(const uint8_t *packet, uint16_t packet_len)
{
    if (!packet || packet_len < RIVR_BLE_FRAG_HDR_LEN) {
        return false;
    }

    return packet[0] == RIVR_BLE_FRAG_MAGIC0 &&
           packet[1] == RIVR_BLE_FRAG_MAGIC1 &&
           packet[2] == RIVR_BLE_FRAG_VERSION;
}

static rivr_ble_frag_rx_slot_t *rivr_ble_frag_find_slot(rivr_ble_frag_rx_t *rx,
                                                         uint8_t msg_id)
{
    uint8_t i;

    for (i = 0u; i < RIVR_BLE_FRAG_RX_SLOTS; ++i) {
        if (rx->slots[i].active && rx->slots[i].msg_id == msg_id) {
            return &rx->slots[i];
        }
    }

    return NULL;
}

static rivr_ble_frag_rx_slot_t *rivr_ble_frag_alloc_slot(rivr_ble_frag_rx_t *rx)
{
    uint8_t i;

    for (i = 0u; i < RIVR_BLE_FRAG_RX_SLOTS; ++i) {
        if (!rx->slots[i].active) {
            return &rx->slots[i];
        }
    }

    if (rx->next_slot >= RIVR_BLE_FRAG_RX_SLOTS) {
        rx->next_slot = 0u;
    }

    i = rx->next_slot;
    rx->next_slot = (uint8_t)((rx->next_slot + 1u) % RIVR_BLE_FRAG_RX_SLOTS);
    return &rx->slots[i];
}

static void rivr_ble_frag_start_slot(rivr_ble_frag_rx_slot_t *slot,
                                     uint8_t msg_id,
                                     uint8_t total_len)
{
    slot->active = true;
    slot->msg_id = msg_id;
    slot->total_len = total_len;
    slot->received_len = 0u;
}

static void rivr_ble_frag_drop_slot(rivr_ble_frag_rx_slot_t *slot)
{
    slot->active = false;
    slot->msg_id = 0u;
    slot->total_len = 0u;
    slot->received_len = 0u;
}

rivr_ble_frag_rx_result_t rivr_ble_frag_ingest(rivr_ble_frag_rx_t *rx,
                                               const uint8_t *packet,
                                               uint16_t packet_len,
                                               const uint8_t **out_payload,
                                               uint16_t *out_len)
{
    rivr_ble_frag_rx_slot_t *slot;
    uint8_t msg_id;
    uint8_t offset;
    uint8_t total_len;
    uint16_t frag_len;

    if (out_payload) {
        *out_payload = NULL;
    }
    if (out_len) {
        *out_len = 0u;
    }
    if (!packet || packet_len == 0u) {
        return RIVR_BLE_FRAG_RX_INVALID;
    }
    if (!rivr_ble_frag_is_packet(packet, packet_len)) {
        if (out_payload) {
            *out_payload = packet;
        }
        if (out_len) {
            *out_len = packet_len;
        }
        return RIVR_BLE_FRAG_RX_PASSTHROUGH;
    }
    if (!rx) {
        return RIVR_BLE_FRAG_RX_INVALID;
    }

    msg_id = packet[3];
    offset = packet[4];
    total_len = packet[5];
    frag_len = (uint16_t)(packet_len - RIVR_BLE_FRAG_HDR_LEN);

    if (total_len == 0u) {
        return RIVR_BLE_FRAG_RX_INVALID;
    }
    if (frag_len == 0u || offset >= total_len) {
        return RIVR_BLE_FRAG_RX_INVALID;
    }
    if ((uint16_t)offset + frag_len > total_len) {
        return RIVR_BLE_FRAG_RX_INVALID;
    }

    slot = rivr_ble_frag_find_slot(rx, msg_id);
    if (slot == NULL) {
        if (offset != 0u) {
            return RIVR_BLE_FRAG_RX_INVALID;
        }

        slot = rivr_ble_frag_alloc_slot(rx);
        rivr_ble_frag_start_slot(slot, msg_id, total_len);
    } else if (offset == 0u) {
        rivr_ble_frag_start_slot(slot, msg_id, total_len);
    } else if (slot->total_len != total_len) {
        rivr_ble_frag_drop_slot(slot);
        return RIVR_BLE_FRAG_RX_INVALID;
    }

    if (offset != slot->received_len) {
        rivr_ble_frag_drop_slot(slot);
        return RIVR_BLE_FRAG_RX_INVALID;
    }

    memcpy(&slot->data[offset], &packet[RIVR_BLE_FRAG_HDR_LEN], frag_len);
    slot->received_len = (uint8_t)(offset + frag_len);

    if (slot->received_len < slot->total_len) {
        return RIVR_BLE_FRAG_RX_INCOMPLETE;
    }

    if (out_payload) {
        *out_payload = slot->data;
    }
    if (out_len) {
        *out_len = slot->total_len;
    }
    rivr_ble_frag_drop_slot(slot);
    return RIVR_BLE_FRAG_RX_COMPLETE;
}

bool rivr_ble_frag_send(const uint8_t *payload,
                        uint16_t payload_len,
                        uint16_t link_payload_limit,
                        uint8_t *next_msg_id,
                        rivr_ble_frag_emit_fn emit,
                        void *ctx)
{
    uint8_t packet[RF_MAX_PAYLOAD_LEN];
    uint16_t frag_payload_len;
    uint16_t offset;
    uint8_t msg_id;

    if (!payload || payload_len == 0u || payload_len > RF_MAX_PAYLOAD_LEN || !emit) {
        return false;
    }

    if (link_payload_limit == 0u) {
        return false;
    }

    if (payload_len <= link_payload_limit) {
        return emit(ctx, payload, payload_len);
    }

    if (link_payload_limit <= RIVR_BLE_FRAG_HDR_LEN) {
        return false;
    }

    frag_payload_len = (uint16_t)(link_payload_limit - RIVR_BLE_FRAG_HDR_LEN);
    if (frag_payload_len == 0u) {
        return false;
    }

    msg_id = 1u;
    if (next_msg_id) {
        (*next_msg_id)++;
        msg_id = *next_msg_id;
        if (msg_id == 0u) {
            (*next_msg_id)++;
            msg_id = *next_msg_id;
        }
    }

    for (offset = 0u; offset < payload_len; offset += frag_payload_len) {
        uint16_t chunk_len = (uint16_t)(payload_len - offset);
        uint16_t packet_len;

        if (chunk_len > frag_payload_len) {
            chunk_len = frag_payload_len;
        }

        packet[0] = RIVR_BLE_FRAG_MAGIC0;
        packet[1] = RIVR_BLE_FRAG_MAGIC1;
        packet[2] = RIVR_BLE_FRAG_VERSION;
        packet[3] = msg_id;
        packet[4] = (uint8_t)offset;
        packet[5] = (uint8_t)payload_len;
        memcpy(&packet[RIVR_BLE_FRAG_HDR_LEN], &payload[offset], chunk_len);

        packet_len = (uint16_t)(RIVR_BLE_FRAG_HDR_LEN + chunk_len);
        if (!emit(ctx, packet, packet_len)) {
            return false;
        }
    }

    return true;
}
