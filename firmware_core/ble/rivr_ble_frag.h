#ifndef RIVR_BLE_FRAG_H
#define RIVR_BLE_FRAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "../radio_sx1262.h"

#define RIVR_BLE_FRAG_MAGIC0 0x52u
#define RIVR_BLE_FRAG_MAGIC1 0x42u
#define RIVR_BLE_FRAG_VERSION 0x01u
#define RIVR_BLE_FRAG_HDR_LEN 6u
#define RIVR_BLE_FRAG_RX_SLOTS 4u

typedef enum {
    RIVR_BLE_FRAG_RX_PASSTHROUGH = 0,
    RIVR_BLE_FRAG_RX_INCOMPLETE,
    RIVR_BLE_FRAG_RX_COMPLETE,
    RIVR_BLE_FRAG_RX_INVALID,
} rivr_ble_frag_rx_result_t;

typedef struct {
    bool active;
    uint8_t msg_id;
    uint8_t total_len;
    uint8_t received_len;
    uint8_t data[RF_MAX_PAYLOAD_LEN];
} rivr_ble_frag_rx_slot_t;

typedef struct {
    uint8_t next_slot;
    rivr_ble_frag_rx_slot_t slots[RIVR_BLE_FRAG_RX_SLOTS];
} rivr_ble_frag_rx_t;

typedef bool (*rivr_ble_frag_emit_fn)(void *ctx, const uint8_t *data, uint16_t len);

void rivr_ble_frag_reset(rivr_ble_frag_rx_t *rx);

rivr_ble_frag_rx_result_t rivr_ble_frag_ingest(rivr_ble_frag_rx_t *rx,
                                               const uint8_t *packet,
                                               uint16_t packet_len,
                                               const uint8_t **out_payload,
                                               uint16_t *out_len);

bool rivr_ble_frag_send(const uint8_t *payload,
                        uint16_t payload_len,
                        uint16_t link_payload_limit,
                        uint8_t *next_msg_id,
                        rivr_ble_frag_emit_fn emit,
                        void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_BLE_FRAG_H */
