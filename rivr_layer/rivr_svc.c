/**
 * @file  rivr_svc.c
 * @brief RIVR application-level network service handlers.
 *
 * Each handler is called from sources_rf_rx_drain() in rivr_sources.c after
 * the routing / reliability layer has finished with a received frame.
 * Handlers are fire-and-forget: they print structured log output and, in the
 * case of PKT_MAILBOX, update the in-RAM store — then return immediately.
 *
 * Structured log lines are prefixed with @CHT / @TEL / @MAIL / @ALERT so
 * the Rivr Lab desktop tool can parse them independently of the RIVR_LOG
 * transport level.
 */

#include "rivr_svc.h"
#include "rivr_embed.h"      /* g_my_node_id, g_rivr_metrics, etc. */
#include "rivr_cli.h"        /* rivr_cli_on_chat_rx() (stub on non-client) */
#include "../firmware_core/ble/rivr_ble_companion.h"
#include "../firmware_core/protocol.h"   /* PKT_FLAG_CHANNEL, RIVR_CHAT_CHAN_HDR_LEN */
#include "../firmware_core/rivr_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "RIVR_SVC"

/* ── Global mailbox in-RAM store ─────────────────────────────────────────── */
mailbox_store_t g_mailbox_store;   /* zero-initialised from BSS */

/* ── Internal helpers ────────────────────────────────────────────────────── */

/**
 * Copy at most `maxlen` bytes of `src` into `dst`, sanitising for JSON:
 *   - double-quote → \42 (hex escape avoids confusing the editor)
 *   - backslash    → \\
 *   - control chars (< 0x20) → '.'
 *   - non-ASCII (> 0x7E) → '.'
 * Always NUL-terminates `dst`.
 * Returns number of bytes written (excluding NUL).
 */
static uint8_t svc_json_text(const uint8_t *src, uint8_t srclen,
                              char *dst, uint8_t maxlen)
{
    uint8_t wi = 0u;
    for (uint8_t i = 0u; i < srclen && wi + 2u < maxlen; i++) {
        uint8_t c = src[i];
        if (c == '"') {
            /* escape double-quote: \" */
            if (wi + 3u < maxlen) { dst[wi++] = '\\'; dst[wi++] = '"'; }
        } else if (c == '\\') {
            if (wi + 3u < maxlen) { dst[wi++] = '\\'; dst[wi++] = '\\'; }
        } else if (c < 0x20u || c > 0x7Eu) {
            dst[wi++] = '.';
        } else {
            dst[wi++] = (char)c;
        }
    }
    dst[wi] = '\0';
    return wi;
}

/* ─────────────────────────────────────────────────────────────────────────── *
 * CHAT
 * ─────────────────────────────────────────────────────────────────────────── */

void handle_chat_message(const rivr_pkt_hdr_t *hdr,
                         const uint8_t        *payload,
                         uint8_t               len,
                         int16_t               rssi_dbm)
{
    if (!hdr || !payload || len == 0u) return;

    /* ── Strip optional channel header (PKT_FLAG_CHANNEL = 0x08) ────────── */
    /* When this flag is set the first two bytes are channel_id (u16 LE);    */
    /* the actual text starts at payload[RIVR_CHAT_CHAN_HDR_LEN].            */
    uint16_t     chan_id    = 0u;
    const uint8_t *text_ptr = payload;
    uint8_t       text_len  = len;

    if ((hdr->flags & PKT_FLAG_CHANNEL) != 0u
            && len >= RIVR_CHAT_CHAN_HDR_LEN) {
        chan_id   = (uint16_t)payload[0]
                 | ((uint16_t)payload[1] << 8u);
        text_ptr  = payload + RIVR_CHAT_CHAN_HDR_LEN;
        text_len  = len - RIVR_CHAT_CHAN_HDR_LEN;
    }

    if (text_len == 0u) return;

    /* ── Structured log line ─────────────────────────────────────────────── */
    char msg_buf[RIVR_PKT_MAX_PAYLOAD + 1u];
    svc_json_text(text_ptr, text_len, msg_buf, sizeof(msg_buf));

    printf("@CHT {\"src\":\"0x%08lx\",\"dst\":\"0x%08lx\","
           "\"chan\":%u,\"rssi\":%d,\"len\":%u,\"text\":\"%s\"}\r\n",
           (unsigned long)hdr->src_id,
           (unsigned long)hdr->dst_id,
           (unsigned)chan_id,
           (int)rssi_dbm,
           (unsigned)text_len,
           msg_buf);

    /* ── Serial console display (client builds) ──────────────────────────── */
    rivr_cli_on_chat_rx(hdr->src_id, text_ptr, text_len);
    rivr_ble_companion_push_chat(hdr->src_id, chan_id, text_ptr, text_len);

    RIVR_LOGD(TAG, "[CHAT] src=0x%08lx chan=%u len=%u rssi=%d",
              (unsigned long)hdr->src_id, (unsigned)chan_id,
              (unsigned)text_len, (int)rssi_dbm);
}

/* ─────────────────────────────────────────────────────────────────────────── *
 * TELEMETRY
 * ─────────────────────────────────────────────────────────────────────────── */

void handle_telemetry_publish(const rivr_pkt_hdr_t *hdr,
                              const uint8_t        *payload,
                              uint8_t               len)
{
    if (!hdr || !payload || len < SVC_TELEMETRY_PAYLOAD_LEN) return;

    /* Map unit code to a short ASCII label for the log */
    static const char *const unit_names[] = {
        "none", "C*100", "%RH*100", "mV", "dBm", "ppm*100"
    };

    /* Payload may contain N consecutive 11-byte readings (combined bundle). */
    uint8_t n = len / SVC_TELEMETRY_PAYLOAD_LEN;

    for (uint8_t i = 0u; i < n; i++) {
        const uint8_t *p = payload + (i * SVC_TELEMETRY_PAYLOAD_LEN);

        /* Decode a single reading:
         *   [0–1]  sensor_id  u16 LE
         *   [2–5]  value      i32 LE
         *   [6]    unit_code  u8
         *   [7–10] timestamp  u32 LE  */
        uint16_t sensor_id  = (uint16_t)(p[0])
                            | ((uint16_t)(p[1]) << 8u);
        int32_t  value      = (int32_t)((uint32_t)p[2]
                            | ((uint32_t)p[3] <<  8u)
                            | ((uint32_t)p[4] << 16u)
                            | ((uint32_t)p[5] << 24u));
        uint8_t  unit_code  = p[6];
        uint32_t timestamp  = (uint32_t)p[7]
                            | ((uint32_t)p[8]  <<  8u)
                            | ((uint32_t)p[9]  << 16u)
                            | ((uint32_t)p[10] << 24u);

        const char *unit_str =
            (unit_code < 6u) ? unit_names[unit_code]
                             : (unit_code == UNIT_CUSTOM ? "custom" : "?");

        printf("@TEL {\"src\":\"0x%08lx\",\"sid\":%u,\"val\":%ld,"
               "\"unit\":%u,\"unit_str\":\"%s\",\"ts\":%lu}\r\n",
               (unsigned long)hdr->src_id,
               (unsigned)sensor_id,
               (long)value,
               (unsigned)unit_code,
               unit_str,
               (unsigned long)timestamp);

        rivr_ble_companion_push_telemetry(hdr->src_id, sensor_id, value,
                                          unit_code, timestamp);

        RIVR_LOGD(TAG, "[TEL] src=0x%08lx sid=%u val=%ld unit=%u ts=%lu",
                  (unsigned long)hdr->src_id,
                  (unsigned)sensor_id,
                  (long)value,
                  (unsigned)unit_code,
                  (unsigned long)timestamp);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── *
 * MAILBOX  (store-and-forward)
 * ─────────────────────────────────────────────────────────────────────────── */

void handle_mailbox_store(const rivr_pkt_hdr_t *hdr,
                          const uint8_t        *payload,
                          uint8_t               len,
                          uint32_t              now_ms)
{
    if (!hdr || !payload || len < SVC_MAILBOX_HDR_LEN) return;

    /* Decode 7-byte header:
     *   [0–3]  recipient_id  u32 LE
     *   [4–5]  msg_seq       u16 LE
     *   [6]    flags         u8           */
    uint32_t recipient_id = (uint32_t)payload[0]
                          | ((uint32_t)payload[1] <<  8u)
                          | ((uint32_t)payload[2] << 16u)
                          | ((uint32_t)payload[3] << 24u);
    uint16_t msg_seq      = (uint16_t)(payload[4])
                          | ((uint16_t)(payload[5]) << 8u);
    uint8_t  flags        = payload[6];

    /* Text body immediately follows the fixed header */
    const uint8_t *text_data = payload + SVC_MAILBOX_HDR_LEN;
    uint8_t  text_raw_len    = (uint8_t)(len - SVC_MAILBOX_HDR_LEN);
    if (text_raw_len > SVC_MAILBOX_MAX_TEXT) {
        text_raw_len = SVC_MAILBOX_MAX_TEXT;
    }

    /* Decide whether to store locally */
    bool for_us = (recipient_id == g_my_node_id) || (recipient_id == 0u);

    if (for_us) {
        /* Insert into the ring-buffer store (LRU eviction when full). */
        mailbox_entry_t *slot = &g_mailbox_store.entries[g_mailbox_store.head];

        slot->src_id       = hdr->src_id;
        slot->recipient_id = recipient_id;
        slot->msg_seq      = msg_seq;
        slot->flags        = (uint8_t)(flags | MB_FLAG_NEW);
        slot->text_len     = text_raw_len;
        slot->stored_at_ms = now_ms;
        slot->valid        = true;

        memcpy(slot->text, text_data, text_raw_len);
        slot->text[text_raw_len] = '\0';

        g_mailbox_store.head =
            (uint8_t)((g_mailbox_store.head + 1u) % MB_STORE_CAP);
        if (g_mailbox_store.count < MB_STORE_CAP) {
            g_mailbox_store.count++;
        }
    }

    /* ── Structured log line ─────────────────────────────────────────────── */
    char text_buf[SVC_MAILBOX_MAX_TEXT + 1u];
    svc_json_text(text_data, text_raw_len, text_buf, sizeof(text_buf));

    printf("@MAIL {\"src\":\"0x%08lx\",\"to\":\"0x%08lx\","
           "\"seq\":%u,\"flags\":%u,\"stored\":%s,\"text\":\"%s\"}\r\n",
           (unsigned long)hdr->src_id,
           (unsigned long)recipient_id,
           (unsigned)msg_seq,
           (unsigned)flags,
           for_us ? "true" : "false",
           text_buf);

    RIVR_LOGD(TAG, "[MAIL] src=0x%08lx to=0x%08lx seq=%u stored=%d",
              (unsigned long)hdr->src_id,
              (unsigned long)recipient_id,
              (unsigned)msg_seq,
              (int)for_us);
}

/* ─────────────────────────────────────────────────────────────────────────── *
 * ALERT
 * ─────────────────────────────────────────────────────────────────────────── */

void handle_alert_event(const rivr_pkt_hdr_t *hdr,
                        const uint8_t        *payload,
                        uint8_t               len)
{
    if (!hdr || !payload || len < SVC_ALERT_PAYLOAD_LEN) return;

    /* Decode 7-byte payload:
     *   [0]    severity    u8
     *   [1–2]  alert_code  u16 LE
     *   [3–6]  value       i32 LE  */
    uint8_t  severity   = payload[0];
    uint16_t alert_code = (uint16_t)(payload[1])
                        | ((uint16_t)(payload[2]) << 8u);
    int32_t  value      = (int32_t)((uint32_t)payload[3]
                        | ((uint32_t)payload[4] <<  8u)
                        | ((uint32_t)payload[5] << 16u)
                        | ((uint32_t)payload[6] << 24u));

    static const char *const sev_names[] = { "?", "INFO", "WARN", "CRIT" };
    const char *sev_str =
        (severity >= 1u && severity <= 3u) ? sev_names[severity] : "?";

    /* ── Structured log line ─────────────────────────────────────────────── */
    printf("@ALERT {\"src\":\"0x%08lx\",\"sev\":%u,\"sev_str\":\"%s\","
           "\"code\":%u,\"val\":%ld}\r\n",
           (unsigned long)hdr->src_id,
           (unsigned)severity,
           sev_str,
           (unsigned)alert_code,
           (long)value);

    /* Human-readable warning for WARN / CRIT severity */
    if (severity >= ALERT_SEV_WARN) {
        printf("[!] ALERT %s from 0x%08lx code=%u val=%ld\r\n",
               sev_str,
               (unsigned long)hdr->src_id,
               (unsigned)alert_code,
               (long)value);
    }

    if (severity >= ALERT_SEV_CRIT) {
        RIVR_LOGW(TAG, "[ALERT] CRITICAL src=0x%08lx code=%u val=%ld",
                  (unsigned long)hdr->src_id,
                  (unsigned)alert_code,
                  (long)value);
    } else {
        RIVR_LOGD(TAG, "[ALERT] sev=%u src=0x%08lx code=%u val=%ld",
                  (unsigned)severity,
                  (unsigned long)hdr->src_id,
                  (unsigned)alert_code,
                  (long)value);
    }
}
