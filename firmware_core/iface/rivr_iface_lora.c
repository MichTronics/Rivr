/**
 * @file  rivr_iface_lora.c
 * @brief LoRa transport adapter — push frames onto rf_tx_queue.
 */

#include "rivr_iface_lora.h"
#include "firmware_core/radio_sx1262.h"
#include "firmware_core/ringbuf.h"
#include <string.h>

bool rivr_iface_lora_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0u || len > RF_MAX_PAYLOAD_LEN) {
        return false;
    }

    rf_tx_request_t req;
    memset(&req, 0, sizeof(req));
    memcpy(req.data, data, len);
    req.len    = (uint8_t)len;
    req.toa_us = RF_TOA_APPROX_US((uint8_t)len);
    req.due_ms = 0u;  /* send at the next tx_drain_loop opportunity */

    return rb_try_push(&rf_tx_queue, &req);
}
