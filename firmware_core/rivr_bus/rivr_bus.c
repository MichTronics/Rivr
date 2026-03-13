/**
 * @file  rivr_bus.c
 * @brief Rivr multi-transport packet bus implementation.
 */

#include "rivr_bus.h"
#include "rivr_dup_cache.h"
#include "rivr_dispatch.h"
#include "firmware_core/iface/rivr_iface_lora.h"
#include "firmware_core/iface/rivr_iface_ble.h"
#include "firmware_core/iface/rivr_iface_usb.h"
#include "firmware_core/protocol.h"
#include "firmware_core/radio_sx1262.h"   /* RF_MAX_PAYLOAD_LEN              */
#include "firmware_core/rivr_metrics.h"
#include "firmware_core/rivr_log.h"
#include "esp_log.h"
#include <string.h>

#define TAG "BUS"

/* ── Module state (BSS zero-initialised) ────────────────────────────────── */
static rivr_bus_dup_cache_t s_dup_cache;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void rivr_bus_init(void)
{
    memset(&s_dup_cache, 0, sizeof(s_dup_cache));
    RIVR_LOGI(TAG, "bus init: dup_cache=%u entries (%u bytes)",
              (unsigned)RIVR_BUS_DUP_CACHE_SIZE,
              (unsigned)sizeof(s_dup_cache));
}

/* ── Internal: per-iface RX counter ─────────────────────────────────────── */

static void bus_count_rx_iface(rivr_iface_t iface)
{
    switch (iface) {
        case RIVR_IFACE_LORA:
            g_rivr_metrics.lora_rx_frames++;
            break;
        case RIVR_IFACE_USB:
            g_rivr_metrics.usb_rx_frames++;
            break;
        case RIVR_IFACE_BLE:
            /* ble_rx_frames is already incremented by rivr_ble_service.c   *
             * when the characteristic is written; do not double-count here. */
            break;
        default:
            break;  /* WIFI / LOCAL — future */
    }
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

bool rivr_bus_dispatch(const uint8_t *frame, size_t len,
                       rivr_iface_mask_t targets, rivr_iface_t source_iface)
{
    if (!frame || len == 0u || targets == RIVR_MASK_NONE) {
        return false;
    }

    bool any = false;
    rivr_iface_mask_t source_bit = (rivr_iface_mask_t)(1u << (uint32_t)source_iface);

    for (uint32_t bit = 0u; bit < (uint32_t)RIVR_IFACE_COUNT; bit++) {
        rivr_iface_mask_t mask = (rivr_iface_mask_t)(1u << bit);
        if ((targets & mask) == 0u) {
            continue;  /* this iface not in target set */
        }
        if (mask == source_bit) {
            continue;  /* never echo back to source transport */
        }

        bool ok = false;
        switch ((rivr_iface_t)bit) {
            case RIVR_IFACE_LORA:
                ok = rivr_iface_lora_send(frame, len);
                if (ok) { g_rivr_metrics.bus_forward_lora++; }
                break;

            case RIVR_IFACE_BLE:
                ok = rivr_iface_ble_send(frame, len);
                if (ok) {
                    g_rivr_metrics.bus_forward_ble++;
                    g_rivr_metrics.ble_tx_frames++;  /* mirrors original BLE notify counter */
                }
                break;

            case RIVR_IFACE_USB:
                ok = rivr_iface_usb_send(frame, len);
                if (ok) { g_rivr_metrics.bus_forward_usb++; }
                break;

            case RIVR_IFACE_WIFI:
                /* Not yet implemented */
                break;

            case RIVR_IFACE_LOCAL:
                /* LOCAL is a virtual source, not a real transmit path */
                break;

            default:
                break;
        }

        if (!ok && mask != (rivr_iface_mask_t)(1u << RIVR_IFACE_WIFI)
                && mask != (rivr_iface_mask_t)(1u << RIVR_IFACE_LOCAL)) {
            /* Count dispatch errors only for real physical transports */
            g_rivr_metrics.bus_errors++;
        }
        if (ok) { any = true; }
    }

    return any;
}

/* ── Ingress ─────────────────────────────────────────────────────────────── */

bool rivr_bus_receive(const uint8_t *data, size_t len,
                      const rivr_rx_meta_t *meta)
{
    /* ── 1. Basic length bounds ──────────────────────────────────────────── */
    if (!data || !meta || len < RIVR_PKT_MIN_FRAME || len > RF_MAX_PAYLOAD_LEN) {
        g_rivr_metrics.bus_drop_invalid++;
        return false;
    }

    /* ── 2. Header decode (lightweight — just need src_id + pkt_id + type) ─ */
    rivr_pkt_hdr_t hdr;
    const uint8_t *payload_ptr = NULL;
    if (!protocol_decode(data, (uint8_t)len, &hdr, &payload_ptr)) {
        g_rivr_metrics.bus_drop_invalid++;
        return false;
    }

    /* ── 3. Cross-transport duplicate cache ─────────────────────────────── */
    if (rivr_bus_dup_cache_seen(&s_dup_cache, hdr.src_id, hdr.pkt_id)) {
        g_rivr_metrics.bus_drop_dup++;
        ESP_LOGD(TAG, "bus: cross-iface dup src=0x%08lx pkt_id=%u iface=%u",
                 (unsigned long)hdr.src_id, (unsigned)hdr.pkt_id,
                 (unsigned)meta->iface);
        return false;
    }
    rivr_bus_dup_cache_add(&s_dup_cache, hdr.src_id, hdr.pkt_id);

    /* ── 4. Per-interface RX counter ────────────────────────────────────── */
    bus_count_rx_iface(meta->iface);
    g_rivr_metrics.bus_rx_total++;

    /* ── 5. Compute egress mask and fan out ─────────────────────────────── */
    rivr_iface_mask_t targets = rivr_dispatch_select(hdr.pkt_type, meta->iface);
    if (targets != RIVR_MASK_NONE) {
        rivr_bus_dispatch(data, len, targets, meta->iface);
    }

    return true;
}

/* ── Local frame injection ───────────────────────────────────────────────── */

bool rivr_bus_send_local(const uint8_t *data, size_t len)
{
    rivr_rx_meta_t meta = {
        .iface        = RIVR_IFACE_LOCAL,
        .rssi         = 0,
        .snr          = 0,
        .timestamp_ms = 0u,
        .flags        = 0u,
    };
    return rivr_bus_receive(data, len, &meta);
}
