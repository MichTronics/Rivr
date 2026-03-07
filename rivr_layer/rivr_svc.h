/**
 * @file  rivr_svc.h
 * @brief RIVR application-level network services.
 *
 * This header declares the four standard service handlers that sit above
 * the routing / reliability layer:
 *
 *   ┌──────────────────────────────────────────────────────────────────────┐
 *   │  Service           │ PKT type       │ Direction            │ ID      │
 *   ├──────────────────────────────────────────────────────────────────────┤
 *   │  CHAT              │ PKT_CHAT  (1)  │ broadcast or unicast │ text    │
 *   │  TELEMETRY         │ PKT_TELEMETRY (8) │ broadcast          │ sensor │
 *   │  STORE_FORWARD_MAILBOX │ PKT_MAILBOX (9) │ unicast / store │ mail   │
 *   │  ALERT             │ PKT_ALERT (10) │ broadcast or unicast │ prio   │
 *   └──────────────────────────────────────────────────────────────────────┘
 *
 * DISPATCH MODEL
 * ──────────────
 * rivr_sources.c calls the appropriate handler for each received frame after
 * the routing layer has processed it.  Handlers never block, never allocate,
 * and always return promptly.  Frames continue to flow into the RIVR engine
 * and the relay path unchanged.
 *
 * MAILBOX STORAGE
 * ───────────────
 * An in-RAM static store of MB_STORE_CAP (8) entries.  Frames addressed to
 * this node (or broadcast, dst_id == 0) are kept in g_mailbox_store.
 * Entries are evicted in LRU order (ring-buffer); no NVS persistence in the
 * base implementation.
 *
 * STRUCTURED LOG OUTPUT
 * ─────────────────────
 * Each handler emits a single-line JSON-formatted @-prefixed log record that
 * the host tool can parse:
 *
 *   @CHT  {"src":"<hex>","rssi":<dBm>,"text":"<msg>"}
 *   @TEL  {"src":"<hex>","sid":<id>,"val":<scaled>,"unit":<code>,"ts":<s>}
 *   @MAIL {"src":"<hex>","to":"<hex>","seq":<n>,"flags":<n>,"text":"<msg>"}
 *   @ALERT{"src":"<hex>","sev":<1-3>,"code":<n>,"val":<n>}
 */

#ifndef RIVR_SVC_H
#define RIVR_SVC_H

#include <stdint.h>
#include <stdbool.h>
#include "../firmware_core/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Mailbox in-RAM store ─────────────────────────────────────────────────── */

#define MB_STORE_CAP  8u   /**< Maximum simultaneously held mailbox messages  */

/** One entry in the in-RAM mailbox store. */
typedef struct {
    uint32_t src_id;                          /**< Sender node ID             */
    uint32_t recipient_id;                    /**< Intended final recipient   */
    uint16_t msg_seq;                         /**< Per-origin message counter */
    uint8_t  flags;                           /**< MB_FLAG_* bitmask          */
    uint8_t  text_len;                        /**< Bytes in text[] (not incl. NUL) */
    char     text[SVC_MAILBOX_MAX_TEXT + 1u]; /**< NUL-terminated body        */
    uint32_t stored_at_ms;                    /**< tb_millis() at insertion   */
    bool     valid;                           /**< Slot occupied flag         */
} mailbox_entry_t;

/** Global in-RAM mailbox — defined in rivr_svc.c, accessible via this extern. */
typedef struct {
    mailbox_entry_t entries[MB_STORE_CAP];
    uint8_t         head;   /**< Next write index (ring-buffer cursor)       */
    uint8_t         count;  /**< Number of valid entries currently stored    */
} mailbox_store_t;

extern mailbox_store_t g_mailbox_store;

/* ── Service handlers ────────────────────────────────────────────────────── *
 *
 * All handlers are called from rivr_sources.c after the routing pipeline    *
 * has processed (and optionally relayed) the frame.  They must not modify   *
 * shared state that has not been explicitly guarded.                        *
 * ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Handle a received PKT_CHAT frame.
 *
 * Displays the message on the serial console (client builds only) and emits
 * a @CHT JSON log line on all builds.
 *
 * @param hdr       Decoded packet header.
 * @param payload   Pointer to payload bytes (NOT NUL-terminated).
 * @param len       Payload length (must be > 0).
 * @param rssi_dbm  Received RSSI for the log record.
 */
void handle_chat_message(const rivr_pkt_hdr_t *hdr,
                         const uint8_t        *payload,
                         uint8_t               len,
                         int16_t               rssi_dbm);

/**
 * @brief Handle a received PKT_TELEMETRY frame.
 *
 * Decodes the fixed-layout telemetry payload and emits a @TEL JSON record.
 * No additional routing or storage is performed.
 *
 * @param hdr     Decoded packet header.
 * @param payload Payload bytes (must be ≥ SVC_TELEMETRY_PAYLOAD_LEN bytes).
 * @param len     Actual payload length.
 */
void handle_telemetry_publish(const rivr_pkt_hdr_t *hdr,
                              const uint8_t        *payload,
                              uint8_t               len);

/**
 * @brief Handle a received PKT_MAILBOX frame.
 *
 * If the frame is addressed to this node (recipient_id == g_my_node_id) or
 * is a broadcast mailbox (recipient_id == 0), the message body is stored in
 * g_mailbox_store (LRU eviction when full) and a @MAIL record is printed.
 * Otherwise the frame is logged as an in-transit forward with @MAIL "fwd"
 * set but is not stored locally.
 *
 * @param hdr     Decoded packet header.
 * @param payload Payload bytes (must be ≥ SVC_MAILBOX_HDR_LEN bytes).
 * @param len     Actual payload length.
 * @param now_ms  Current monotonic timestamp from tb_millis().
 */
void handle_mailbox_store(const rivr_pkt_hdr_t *hdr,
                          const uint8_t        *payload,
                          uint8_t               len,
                          uint32_t              now_ms);

/**
 * @brief Handle a received PKT_ALERT frame.
 *
 * Decodes the 7-byte fixed payload, emits a @ALERT JSON record, and writes
 * a human-readable warning to the serial console for WARN and CRIT levels.
 *
 * @param hdr     Decoded packet header.
 * @param payload Payload bytes (must be ≥ SVC_ALERT_PAYLOAD_LEN bytes).
 * @param len     Actual payload length.
 */
void handle_alert_event(const rivr_pkt_hdr_t *hdr,
                        const uint8_t        *payload,
                        uint8_t               len);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_SVC_H */
