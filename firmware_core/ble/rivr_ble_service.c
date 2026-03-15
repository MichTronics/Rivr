/**
 * @file  rivr_ble_service.c
 * @brief Rivr BLE GATT service: RX write handler + TX notify.
 *
 * SERVICE DESIGN (Nordic UART Service UUIDs for universal app compatibility)
 * ──────────────────────────────────────────────────────────────────────────
 *  Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *    TX chr 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  Notify  (node → phone)
 *    RX chr 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  Write   (phone → node)
 *
 * RX PATH  (phone → node)
 * ────────────────────────
 *  1. Phone writes a binary Rivr frame to the RX characteristic.
 *  2. rivr_chr_access_cb() copies it into an rf_rx_frame_t.
 *  3. rb_try_push(&rf_rx_ringbuf, &frame) injects it into the shared
 *     receive ring buffer — identical to a frame arriving from LoRa.
 *  4. On the next main-loop iteration, sources_rf_rx_drain() processes it
 *     through protocol_decode, dedupe, routing, and the RIVR engine.
 *
 * TX PATH  (node → phone)
 * ────────────────────────
 *  rivr_ble_service_notify() is called from sources_rf_rx_drain() whenever
 *  a well-formed, non-deduplicated frame arrives from the radio.  The phone
 *  gets a forwarded copy of all live mesh traffic + any originated frames.
 *
 * THREAD SAFETY
 * ─────────────
 *  rivr_chr_access_cb   — NimBLE host task (producer to rf_rx_ringbuf)
 *  rivr_ble_service_notify — main-loop task (calls ble_gatts_notify_custom
 *                             which acquires NimBLE's internal lock)
 *  rf_rx_ringbuf is an SPSC atomic ring buffer — no mutex needed.
 */

#include "rivr_ble_service.h"
#include "rivr_config.h"

#if RIVR_FEATURE_BLE

#include <string.h>
#include "esp_log.h"

/* NimBLE headers */
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

/* Rivr firmware headers */
#include "../radio_sx1262.h"   /* rf_rx_ringbuf, rf_rx_frame_t, RF_MAX_PAYLOAD_LEN */
#include "../rivr_metrics.h"   /* g_rivr_metrics.ble_*                             */
#include "../rivr_log.h"
#include "../timebase.h"       /* tb_millis()                                      */
#include "../ringbuf.h"

#define TAG "RIVR_BLE_SVC"

/* ── 128-bit UUIDs (stored little-endian as required by NimBLE) ──────────── *
 *  6E400001-B5A3-F393-E0A9-E50E24DCCA9E → bytes[0] = 0x9E, bytes[15] = 0x6E */

/* Service UUID: 6E400001-... */
static const ble_uuid128_t s_rivr_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

/* RX characteristic: 6E400002-... (Write / Write Without Response, phone → node) */
static const ble_uuid128_t s_rivr_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

/* TX characteristic: 6E400003-... (Notify, node → phone) */
static const ble_uuid128_t s_rivr_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* ── Attribute handle cache ──────────────────────────────────────────────── */

/** Handle for the TX characteristic value attribute.
 *  Populated by NimBLE after ble_gatts_add_svcs() completes.
 *  Used by rivr_ble_service_notify() to address the correct attribute. */
static uint16_t s_tx_chr_val_handle = 0u;

/* ── GATT access callback (called from NimBLE host task) ─────────────────── */

/**
 * @brief Combined GATT access callback for both characteristics.
 *
 * TX characteristic: read operations return 0 bytes (notify-only).
 * RX characteristic: write operations push the payload into rf_rx_ringbuf.
 */
static int rivr_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* TX characteristic supports only notify — no readable value. */
        return 0;

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        /* ── Phone → node: copy mbuf into rf_rx_ringbuf ──────────────── */
        uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);

        if (pkt_len == 0u || pkt_len > RF_MAX_PAYLOAD_LEN) {
            RIVR_LOGW(TAG, "BLE rx: frame length %u out of range (max=%u)",
                      (unsigned)pkt_len, (unsigned)RF_MAX_PAYLOAD_LEN);
            g_rivr_metrics.ble_errors++;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        rf_rx_frame_t frame;
        memset(&frame, 0, sizeof(frame));

        /* Copy from OS mbuf chain into flat buffer */
        uint16_t clen = 0u;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, frame.data,
                                     sizeof(frame.data), &clen);
        if (rc != 0) {
            RIVR_LOGW(TAG, "BLE rx: mbuf flatten failed (rc=%d)", rc);
            g_rivr_metrics.ble_errors++;
            return BLE_ATT_ERR_UNLIKELY;
        }

        frame.len        = (uint8_t)clen;
        frame.rssi_dbm   = 0;      /* BLE has no RSSI equivalent here    */
        frame.snr_db     = 0;
        frame.rx_mono_ms = tb_millis();
        frame.from_id    = 0u;     /* 0 = local/BLE origin; not a relay  */
        frame.iface      = 1u;     /* RIVR_IFACE_BLE — identifies transport
                                    * to the bus for dispatch + dup cache  */

        /* Push into the shared SPSC receive ring buffer.
         * Producer = this function (NimBLE task).
         * Consumer = sources_rf_rx_drain() (main-loop task). */
        if (!rb_try_push(&rf_rx_ringbuf, &frame)) {
            RIVR_LOGW(TAG, "BLE rx: rf_rx_ringbuf full — frame dropped");
            g_rivr_metrics.ble_errors++;
            /* Return success to the client anyway — the drop is our problem */
        } else {
            g_rivr_metrics.ble_rx_frames++;
            RIVR_LOGI(TAG, "BLE rx: %u bytes pushed to rf_rx_ringbuf",
                      (unsigned)clen);
        }
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ── GATT service table ──────────────────────────────────────────────────── */

static const struct ble_gatt_svc_def s_rivr_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_rivr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* TX characteristic — node → phone (Notify).
                 * BLE_GATT_CHR_F_READ_ENC requires an encrypted link before
                 * NimBLE permits the client to subscribe (CCCD write), so the
                 * phone must complete pairing before notifications flow.    */
                .uuid       = &s_rivr_tx_uuid.u,
                .access_cb  = rivr_chr_access_cb,
                .val_handle = &s_tx_chr_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY
#if RIVR_BLE_PASSKEY != 0
                            | BLE_GATT_CHR_F_READ_ENC
#endif
                ,
            },
            {
                /* RX characteristic — phone → node (Write / Write-NR).
                 * BLE_GATT_CHR_F_WRITE_ENC rejects frames from unauthenticated
                 * clients with ATT_ERR_INSUFFICIENT_ENC (0x0F).            */
                .uuid      = &s_rivr_rx_uuid.u,
                .access_cb = rivr_chr_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
#if RIVR_BLE_PASSKEY != 0
                           | BLE_GATT_CHR_F_WRITE_ENC
#endif
                ,
            },
            { 0 }  /* characteristic list terminator */
        },
    },
    { 0 }  /* service list terminator */
};

/* ── Public API ──────────────────────────────────────────────────────────── */

int rivr_ble_service_register(void)
{
    int rc;

    /* Count the required attribute handles */
    rc = ble_gatts_count_cfg(s_rivr_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return rc;
    }

    /* Register the service table */
    rc = ble_gatts_add_svcs(s_rivr_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return rc;
    }

    RIVR_LOGI(TAG, "GATT service registered (TX handle will be assigned on sync)");
    return 0;
}

void rivr_ble_service_notify(uint16_t conn_handle,
                              const uint8_t *data, uint8_t len)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (s_tx_chr_val_handle == 0u) return;  /* service not yet synced */
    if (len == 0u) return;

    /* Allocate an OS mbuf and copy the frame in.
     * ble_hs_mbuf_from_flat() returns NULL if the NimBLE buffer pool is
     * exhausted (memory pressure).  We count this as a BLE error and skip. */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (!om) {
        RIVR_LOGW(TAG, "BLE notify: mbuf alloc failed (len=%u)", (unsigned)len);
        g_rivr_metrics.ble_errors++;
        return;
    }

    /* ble_gatts_notify_custom() transfers ownership of @p om to NimBLE.
     * Non-zero return means the client unsubscribed or the connection was
     * torn down — count it for diagnostics but do not crash. */
    int rc = ble_gatts_notify_custom(conn_handle, s_tx_chr_val_handle, om);
    if (rc == 0) {
        g_rivr_metrics.ble_tx_frames++;
    } else {
        /* BLE_HS_ENOTCONN or BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND):
         * connection dropped between the check and the send. */
        g_rivr_metrics.ble_errors++;
        RIVR_LOGD(TAG, "BLE notify: send failed (rc=%d conn=0x%04x)",
                  rc, (unsigned)conn_handle);
    }
}

#endif /* RIVR_FEATURE_BLE */
