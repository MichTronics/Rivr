/**
 * @file  private_chat.h
 * @brief RIVR 1-to-1 private chat engine.
 *
 * WIRE FORMAT (PKT_PRIVATE_CHAT payload, max 230 bytes)
 * ──────────────────────────────────────────────────────
 *  [0–7]   msg_id          u64 LE  stable unique message ID (from + sender_seq)
 *  [8–11]  sender_seq      u32 LE  per-sender monotonic counter (full 32-bit)
 *  [12–15] timestamp_s     u32 LE  sender Unix time in seconds (0 = unknown)
 *  [16–17] flags           u16 LE  PCHAT_FLAG_* bitmask
 *  [18–19] expires_delta_s u16 LE  0=no expiry; else seconds after timestamp_s
 *  [20–23] recipient_id    u32 LE  final destination node ID
 *  [24–27] reserved        u32 LE  reserved for future reply/thread metadata
 *  [28]    body_len        u8      byte count of body (0..PRIVATE_CHAT_MAX_BODY)
 *  [29..]  body            UTF-8   message text
 *
 * WIRE FORMAT (PKT_DELIVERY_RECEIPT payload, fixed 17 bytes)
 * ───────────────────────────────────────────────────────────
 *  [0–7]   orig_msg_id     u64 LE  msg_id being acknowledged
 *  [8–11]  sender_id       u32 LE  original message sender node ID
 *  [12–15] timestamp_s     u32 LE  receipt generation time (0 = unknown)
 *  [16]    status          u8      RCPT_STATUS_* value
 *
 * DELIVERY STATE MACHINE (outgoing messages)
 * ────────────────────────────────────────────
 *  QUEUED ──route-hit──► SENT ──wire-ACK──► FORWARDED ──receipt──► DELIVERED
 *  QUEUED ──no-route──► AWAITING_ROUTE ──route──► SENT
 *  QUEUED / AWAITING_ROUTE ──expiry──► EXPIRED
 *  SENT / FORWARDED ──retry-budget-gone──► FAILED_RETRY
 *  AWAITING_ROUTE ──expiry──► FAILED_NO_ROUTE
 *  FORWARDED ──receipt-timeout──► DELIVERY_UNCONFIRMED
 *
 * MEMORY
 * ──────
 * All state in BSS (PRIVATE_CHAT_QUEUE_SIZE × sizeof(pchat_entry_t) + dedup
 * cache).  No heap.  No dynamic allocation in hot paths.
 *
 * ADDRESSING
 * ──────────
 * The outer Rivr header's dst_id is the current unicast next hop. The final
 * private-chat destination therefore lives in the payload as recipient_id so
 * intermediate relays can ACK and forward hop-by-hop without consuming the
 * message locally.
 *
 * COMPATIBILITY
 * ─────────────
 * Old firmware that does not recognise PKT_PRIVATE_CHAT (type=12):
 *   • relay nodes: routing_flood_forward() returns RIVR_FWD_DROP_INVALID (new
 *     type > PKT_METRICS is accepted with RIVR_PROTO_MIN/MAX guards — see note).
 *   • destination node: protocol_decode() will set pkt_type and pass through;
 *     the application layer calls private_chat_on_rx() per NEW_PKT_TYPE rules.
 *   • For mixed-version fleets the sender must set PCHAT_FLAG_STORE_FWD_OK=0
 *     so old relay nodes that forward-drop produce a fast failure signal instead
 *     of silent black-hole.
 *
 * NOTE: PKT_TYPE guards — on receipt of type 12/13 an old node with
 *   rx_invalid_type check (type > PKT_METRICS) will increment that counter and
 *   drop the frame.  This is the intended degradation path: the sender's retry
 *   budget exhausts and reports FAILED_RETRY.  No silent discard ambiguity.
 */

#ifndef RIVR_PRIVATE_CHAT_H
#define RIVR_PRIVATE_CHAT_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"
#include "rivr_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Size limits ─────────────────────────────────────────────────────────── */

#define PRIVATE_CHAT_PAYLOAD_HDR_LEN  29u   /**< Fixed bytes before body in payload */
#define PRIVATE_CHAT_MAX_BODY         200u  /**< Max UTF-8 body bytes               */
#define PRIVATE_CHAT_MAX_PAYLOAD_LEN  \
    ((uint8_t)(PRIVATE_CHAT_PAYLOAD_HDR_LEN + PRIVATE_CHAT_MAX_BODY))

#define DELIVERY_RECEIPT_PAYLOAD_LEN  17u   /**< Fixed receipt payload size          */

/** Compile-time guard: ensure payload fits in RIVR_PKT_MAX_PAYLOAD. */
_Static_assert(PRIVATE_CHAT_MAX_PAYLOAD_LEN <= RIVR_PKT_MAX_PAYLOAD,
               "PRIVATE_CHAT_MAX_PAYLOAD_LEN exceeds RIVR_PKT_MAX_PAYLOAD");
_Static_assert(DELIVERY_RECEIPT_PAYLOAD_LEN <= RIVR_PKT_MAX_PAYLOAD,
               "DELIVERY_RECEIPT_PAYLOAD_LEN exceeds RIVR_PKT_MAX_PAYLOAD");

/* ── Queue / timing limits ───────────────────────────────────────────────── */

#define PRIVATE_CHAT_QUEUE_SIZE       8u      /**< Max simultaneous outgoing msgs    */
#define PRIVATE_CHAT_QUEUE_EXPIRY_MS  30000u  /**< Max enqueue time before failure   */
#define PRIVATE_CHAT_RECEIPT_TMO_MS   60000u  /**< Max sent→receipt before UNCONFIRMED */
#define PRIVATE_CHAT_DEDUP_SIZE       32u     /**< RX dedup cache slots              */
#define PRIVATE_CHAT_RATE_INTERVAL_MS 5000u   /**< Min ms between sends to same peer */
#define PRIVATE_CHAT_ROUTE_REQ_RETRY_MS 5000u /**< Re-issue ROUTE_REQ while awaiting route */
#define PRIVATE_CHAT_RECEIPT_RATE_MAX 4u      /**< Max receipts per time window      */
#define PRIVATE_CHAT_RECEIPT_RATE_WIN_MS 10000u /**< Receipt rate window (ms)        */

/* ── Flags ───────────────────────────────────────────────────────────────── */

/** Private chat payload flag bits (16-bit bitmask on wire). */
#define PCHAT_FLAG_ACK_REQUIRED    0x0001u  /**< Request wire-level ACK from next hop */
#define PCHAT_FLAG_RECEIPT_REQ     0x0002u  /**< Request end-to-end delivery receipt  */
#define PCHAT_FLAG_RETRANSMIT      0x0004u  /**< This is a retransmit of the same msg */
#define PCHAT_FLAG_STORE_FWD_OK    0x0008u  /**< Intermediate relay may buffer        */
#define PCHAT_FLAG_USER_VISIBLE    0x0010u  /**< Should surface in UI                 */
#define PCHAT_FLAG_SYSTEM_GENERTED 0x0020u  /**< Generated by firmware, not user      */

/** Default flags for a user-originated private message. */
#define PCHAT_FLAGS_DEFAULT \
    (PCHAT_FLAG_ACK_REQUIRED | PCHAT_FLAG_RECEIPT_REQ | PCHAT_FLAG_USER_VISIBLE)

/* ── Receipt status codes ────────────────────────────────────────────────── */

#define RCPT_STATUS_DELIVERED      0x00u  /**< Message accepted by app at destination */
#define RCPT_STATUS_REJECTED       0x01u  /**< Destination rejected (e.g. spam, full) */
#define RCPT_STATUS_EXPIRED        0x02u  /**< Expired at destination before delivery */
#define RCPT_STATUS_UNSUPPORTED    0x03u  /**< Destination does not support private chat */

/* ── Delivery states ─────────────────────────────────────────────────────── */

typedef enum {
    PCHAT_STATE_QUEUED              = 0,  /**< Added locally; not yet transmitted     */
    PCHAT_STATE_AWAITING_ROUTE      = 1,  /**< No route; waiting for ROUTE_RPL        */
    PCHAT_STATE_SENT                = 2,  /**< Frame pushed to TX queue               */
    PCHAT_STATE_FORWARDED           = 3,  /**< Wire-level ACK received from next hop  */
    PCHAT_STATE_DELIVERED           = 4,  /**< End-to-end delivery receipt received   */
    PCHAT_STATE_DELIVERY_UNCONFIRMED= 5,  /**< ACK received but receipt timed out     */
    PCHAT_STATE_FAILED_NO_ROUTE     = 6,  /**< Route never resolved within expiry     */
    PCHAT_STATE_FAILED_RETRY        = 7,  /**< Retry budget exhausted before delivery */
    PCHAT_STATE_EXPIRED             = 8,  /**< Queue expiry reached                   */
} pchat_delivery_state_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */

typedef enum {
    PCHAT_OK                 = 0,
    PCHAT_ERR_QUEUE_FULL     = -1,
    PCHAT_ERR_BODY_TOO_LONG  = -2,
    PCHAT_ERR_INVALID_PEER   = -3,
    PCHAT_ERR_ENCODE_FAIL    = -4,
    PCHAT_ERR_INVALID_PAYLOAD= -5,
    PCHAT_ERR_DST_MISMATCH   = -6,
    PCHAT_ERR_RATE_LIMITED   = -7,
    PCHAT_ERR_DUPLICATE      = -8,
} pchat_error_t;

/* ── Payload structures (packed for safe memcpy to/from wire buf) ────────── */

/**
 * Private chat payload wire layout — packed to match on-air bytes exactly.
 * Use private_chat_encode() / private_chat_decode() rather than direct cast.
 */
typedef struct __attribute__((packed)) {
    uint64_t msg_id;           /**< Stable unique message identity              */
    uint32_t sender_seq;       /**< Per-sender monotonic counter                */
    uint32_t timestamp_s;      /**< Sender Unix time (seconds); 0=unknown       */
    uint16_t flags;            /**< PCHAT_FLAG_* bitmask                        */
    uint16_t expires_delta_s;  /**< 0=no expiry; else seconds after timestamp_s */
    uint32_t recipient_id;     /**< Final destination node ID                   */
    uint32_t reserved;         /**< Reserved for future reply/thread metadata   */
    uint8_t  body_len;         /**< Byte count of body (0..200)                 */
    uint8_t  body[PRIVATE_CHAT_MAX_BODY]; /**< UTF-8 message text               */
} private_chat_payload_t;

/**
 * Delivery receipt payload wire layout — packed.
 */
typedef struct __attribute__((packed)) {
    uint64_t orig_msg_id;   /**< msg_id of the acknowledged message             */
    uint32_t sender_id;     /**< Node ID of the original message sender         */
    uint32_t timestamp_s;   /**< Receipt generation time (Unix s); 0=unknown    */
    uint8_t  status;        /**< RCPT_STATUS_* value                            */
} delivery_receipt_payload_t;

_Static_assert(sizeof(delivery_receipt_payload_t) == DELIVERY_RECEIPT_PAYLOAD_LEN,
               "delivery_receipt_payload_t size mismatch");

/* ── Outgoing message queue entry ────────────────────────────────────────── */

typedef struct {
    uint64_t                msg_id;           /**< Stable identity                  */
    uint32_t                peer_id;          /**< Destination node ID              */
    uint8_t                 frame[255];       /**< Encoded wire frame               */
    uint8_t                 frame_len;        /**< Frame byte count                 */
    uint32_t                enqueued_ms;      /**< tb_millis() at enqueue           */
    uint32_t                last_route_req_ms;/**< Last ROUTE_REQ emission          */
    uint32_t                sent_ms;          /**< tb_millis() when first sent      */
    uint16_t                pkt_id;           /**< Current wire pkt_id for retry    */
    pchat_delivery_state_t  state;            /**< Current delivery state           */
    bool                    valid;            /**< Slot in use                      */
} pchat_entry_t;

/* ── RX dedup cache entry ────────────────────────────────────────────────── */

typedef struct {
    uint64_t msg_id;      /**< msg_id of a received private message      */
    uint32_t from_id;     /**< sender node ID                            */
    uint32_t seen_ms;     /**< tb_millis() when first seen                */
    bool     valid;
} pchat_dedup_entry_t;

/* ── Receipt rate-limit state ────────────────────────────────────────────── */

typedef struct {
    uint32_t window_start_ms; /**< Start of current rate-limit window    */
    uint8_t  count;           /**< Receipts sent in current window       */
} pchat_receipt_rate_t;

/* ── Module state (extern — defined in private_chat.c) ──────────────────── */

/** Outgoing message queue (BSS). */
extern pchat_entry_t g_pchat_queue[PRIVATE_CHAT_QUEUE_SIZE];

/** RX dedup cache (BSS). */
extern pchat_dedup_entry_t g_pchat_dedup[PRIVATE_CHAT_DEDUP_SIZE];

/* ── Initialisation ──────────────────────────────────────────────────────── */

/**
 * @brief Initialise the private chat engine.
 * Called once from app startup. Safe to call after reboot. Clears all state.
 */
void private_chat_init(void);

/* ── Outgoing API ────────────────────────────────────────────────────────── */

/**
 * @brief Enqueue an outgoing private message.
 *
 * Encodes the payload header, looks up the route, and either:
 *   a) Immediately pushes the frame to rf_tx_queue (route known).
 *   b) Starts route discovery and parks the frame in the pending queue
 *      (PCHAT_STATE_AWAITING_ROUTE).
 *
 * @param peer      Destination node ID (must be non-zero, non-broadcast).
 * @param body      UTF-8 message body (not NUL-terminated).
 * @param body_len  Byte count (0..PRIVATE_CHAT_MAX_BODY).
 * @param out_msg_id Set to the assigned msg_id on success (may be NULL).
 * @return PCHAT_OK or a PCHAT_ERR_* code.
 */
pchat_error_t private_chat_send(uint32_t peer,
                                 const uint8_t *body,
                                 uint8_t body_len,
                                 uint64_t *out_msg_id);

/**
 * @brief Called when a wire-level ACK is received for one of our messages.
 *
 * Transitions the matching entry SENT→FORWARDED.  Does not affect receipt wait.
 * Also called during retry_table_tick() exhaust: call private_chat_on_wire_ack
 * with success=false to mark FAILED_RETRY.
 *
 * @param pkt_id  Acknowledged wire pkt_id (from PKT_ACK payload).
 * @param src_id  Original sender (our local node ID — validated internally).
 * @param success true = ACK received; false = retry exhausted.
 */
void private_chat_on_wire_ack(uint16_t pkt_id, uint32_t src_id, bool success);

/* ── Incoming API ────────────────────────────────────────────────────────── */

/**
 * @brief Process an incoming PKT_PRIVATE_CHAT frame destined for this node.
 *
 * Validates destination, deduplicates, emits delivery receipt, and notifies
 * the companion control plane.
 *
 * @param hdr       Decoded packet header (must have dst_id == our node ID).
 * @param payload   Raw payload bytes from the wire frame.
 * @param pay_len   Byte count of payload.
 * @return PCHAT_OK, PCHAT_ERR_DST_MISMATCH, PCHAT_ERR_INVALID_PAYLOAD,
 *         PCHAT_ERR_DUPLICATE, or PCHAT_ERR_RATE_LIMITED.
 */
pchat_error_t private_chat_on_rx(const rivr_pkt_hdr_t *hdr,
                                  const uint8_t *payload,
                                  uint8_t pay_len);

/**
 * @brief Process an incoming PKT_DELIVERY_RECEIPT frame.
 *
 * Correlates to an outgoing entry via orig_msg_id + sender_id.
 * Transitions matching entry →DELIVERED (or notes rejection/expiry).
 *
 * @param hdr       Decoded header (src_id = receipt sender = original destination).
 * @param payload   Raw payload bytes.
 * @param pay_len   Byte count (must be DELIVERY_RECEIPT_PAYLOAD_LEN).
 * @return PCHAT_OK or error.
 */
pchat_error_t private_chat_on_receipt(const rivr_pkt_hdr_t *hdr,
                                       const uint8_t *payload,
                                       uint8_t pay_len);

/**
 * @brief Rewrite a relayed private-chat/receipt header for the next hop.
 *
 * Transit hops receive PKT_PRIVATE_CHAT / PKT_DELIVERY_RECEIPT with hdr.dst_id
 * set to the current hop. Before re-enqueueing the frame they must resolve the
 * final destination from the payload and rewrite hdr.dst_id to the next hop
 * toward that final node, while keeping hop count progression intact.
 *
 * For PKT_PRIVATE_CHAT the final destination is payload.recipient_id.
 * For PKT_DELIVERY_RECEIPT the final destination is payload.sender_id.
 *
 * @param hdr      Header already mutated for relay (hop incremented, ttl decremented).
 * @param payload  Decoded wire payload bytes.
 * @param pay_len  Payload length in bytes.
 * @param now_ms   Current monotonic timestamp for route-cache lookup.
 * @return true if a next hop was resolved and hdr rewritten; false on decode
 *         failure or route miss.
 */
bool private_chat_prepare_relay_header(rivr_pkt_hdr_t *hdr,
                                       const uint8_t *payload,
                                       uint8_t pay_len,
                                       uint32_t now_ms,
                                       uint32_t *out_final_dst);

/* ── Periodic tick ───────────────────────────────────────────────────────── */

/**
 * @brief Tick the private chat engine — call from the main loop every ~100 ms.
 *
 * Handles:
 *   • expiry of QUEUED / AWAITING_ROUTE / SENT / FORWARDED entries;
 *   • receipt timeout (SENT/FORWARDED → DELIVERY_UNCONFIRMED);
 *   • route-resolved drain (AWAITING_ROUTE → SENT);
 *   • notifying companion of state transitions.
 *
 * @param now_ms  Current tb_millis() value.
 */
void private_chat_tick(uint32_t now_ms);

/* ── Encode / decode helpers ─────────────────────────────────────────────── */

/**
 * @brief Encode a private chat payload into out_buf.
 *
 * @param p         Payload fields (body_len must be ≤ PRIVATE_CHAT_MAX_BODY).
 * @param out_buf   Output buffer of at least PRIVATE_CHAT_MAX_PAYLOAD_LEN bytes.
 * @param out_cap   Buffer capacity.
 * @return Encoded byte count, or -1 on error.
 */
int private_chat_encode_payload(const private_chat_payload_t *p,
                                 uint8_t *out_buf,
                                 uint8_t out_cap);

/**
 * @brief Decode raw payload bytes into a private_chat_payload_t.
 *
 * @param buf       Raw payload bytes.
 * @param len       Byte count (must be >= PRIVATE_CHAT_PAYLOAD_HDR_LEN + body_len).
 * @param out       Output struct.  body[] is NUL-padded but NOT NUL-terminated
 *                  at body_len; caller must treat body[0..body_len-1] as raw bytes.
 * @return PCHAT_OK or PCHAT_ERR_INVALID_PAYLOAD.
 */
pchat_error_t private_chat_decode_payload(const uint8_t *buf,
                                           uint8_t len,
                                           private_chat_payload_t *out);

/**
 * @brief Encode a delivery receipt payload into out_buf (17 bytes).
 */
int private_chat_encode_receipt(const delivery_receipt_payload_t *r,
                                 uint8_t *out_buf,
                                 uint8_t out_cap);

/**
 * @brief Decode a delivery receipt payload.
 */
pchat_error_t private_chat_decode_receipt(const uint8_t *buf,
                                           uint8_t len,
                                           delivery_receipt_payload_t *out);

/* ── Diagnostics ─────────────────────────────────────────────────────────── */

/**
 * @brief Print private chat diagnostics to the log sink (called by @MET or
 *        on demand from the companion diagnostics request).
 */
void private_chat_print_diag(void);

/**
 * @brief Return count of currently occupied outgoing queue slots.
 */
uint8_t private_chat_queue_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_PRIVATE_CHAT_H */
