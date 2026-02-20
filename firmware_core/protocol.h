/**
 * @file  protocol.h
 * @brief RIVR binary on-air packet format (v1).
 *
 * Wire layout (total = RIVR_PKT_HDR_LEN + payload_len + 2 bytes CRC):
 *
 *   [0–1]   magic       u16 LE  (RIVR_MAGIC = 0x5256 = "RV")
 *   [2]     version     u8      (RIVR_PROTO_VER = 1)
 *   [3]     pkt_type    u8      (PKT_*)            ← FilterPktType inspects here
 *   [4]     flags       u8
 *   [5]     ttl         u8      decremented on each hop
 *   [6]     hop         u8      incremented on each hop
 *   [7–8]   net_id      u16 LE  network / channel discriminator
 *   [9–12]  src_id      u32 LE  sender node ID (device unique)
 *   [13–16] dst_id      u32 LE  destination node ID (0 = broadcast)
 *   [17–20] seq         u32 LE  per-source monotonic counter
 *   [21]    payload_len u8      bytes of application payload following header
 *   [22 .. 22+payload_len-1]    payload
 *   [22+payload_len .. +1]      CRC-16/CCITT (LE) over bytes [0 .. 22+payload_len-1]
 *
 * Total minimum wire size = 24 bytes (header + 0-byte payload + CRC).
 * Maximum wire size       = 24 + RIVR_PKT_MAX_PAYLOAD bytes.
 */

#ifndef RIVR_PROTOCOL_H
#define RIVR_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Magic / version ─────────────────────────────────────────────────────── */

#define RIVR_MAGIC         0x5256u   /**< "RV" little-endian u16             */
#define RIVR_PROTO_VER     1u        /**< Protocol version byte              */

/* ── Packet-type constants ───────────────────────────────────────────────── */

#define PKT_CHAT           1u   /**< Text message                            */
#define PKT_BEACON         2u   /**< Periodic node-presence advertisement    */
#define PKT_ROUTE_REQ      3u   /**< Route-request (future)                  */
#define PKT_ROUTE_RPL      4u   /**< Route-reply (future)                    */
#define PKT_ACK            5u   /**< Acknowledgement (future)                */
#define PKT_DATA           6u   /**< Generic sensor data                     */

/* ── Layout constants ────────────────────────────────────────────────────── */

/** Byte offset of the pkt_type field in the serialised wire frame. */
#define PKT_TYPE_BYTE_OFFSET   3u

/** Fixed header length in bytes (before payload). */
#define RIVR_PKT_HDR_LEN       22u

/** CRC appended after payload: 2 bytes. */
#define RIVR_PKT_CRC_LEN       2u

/** Maximum application payload bytes (keeps total frame ≤ 255 bytes). */
#define RIVR_PKT_MAX_PAYLOAD   ((uint8_t)(255u - RIVR_PKT_HDR_LEN - RIVR_PKT_CRC_LEN))

/** Minimum encoded frame size (0-byte payload). */
#define RIVR_PKT_MIN_FRAME     (RIVR_PKT_HDR_LEN + RIVR_PKT_CRC_LEN)

/* ── Default TTL ─────────────────────────────────────────────────────────── */

#define RIVR_PKT_DEFAULT_TTL   7u   /**< Hops before a packet is discarded   */

/* ── Flags bitmask ───────────────────────────────────────────────────────── */

#define PKT_FLAG_ACK_REQ   0x01u   /**< Sender requests an ACK              */
#define PKT_FLAG_RELAY     0x02u   /**< Packet has been relayed              */
#define PKT_FLAG_FALLBACK  0x04u   /**< Unicast failed; re-sent as flood     */

/* ── Fallback-flood TTL ─────────────────────────────────────────────────── *
 * When a unicast TX attempt fails (queue full or route stale), the sender   *
 * re-encodes the frame as broadcast with this reduced TTL so it does not    *
 * propagate as far as a normal flood.                                       *
 * ─────────────────────────────────────────────────────────────────────────── */
#define RIVR_FALLBACK_TTL  3u   /**< TTL for unicast-failed flood fallback   */

/* ── Packet header struct (in-memory, NOT the wire layout) ──────────────── *
 *
 * Use protocol_encode() / protocol_decode() to convert between this struct
 * and the packed wire format.  The struct itself is NOT __packed so the C
 * compiler may insert padding — always use the encode/decode helpers.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t magic;        /**< Always RIVR_MAGIC (checked by decode)        */
    uint8_t  version;      /**< Protocol version                             */
    uint8_t  pkt_type;     /**< PKT_* constant                               */
    uint8_t  flags;        /**< PKT_FLAG_* bitmask                           */
    uint8_t  ttl;          /**< Hops remaining                               */
    uint8_t  hop;          /**< Hops taken so far                            */
    uint16_t net_id;       /**< Network / channel discriminator              */
    uint32_t src_id;       /**< Sender node unique ID                        */
    uint32_t dst_id;       /**< Destination node ID (0 = broadcast)          */
    uint32_t seq;          /**< Per-source monotonic counter                 */
    uint8_t  payload_len;  /**< Length of accompanying payload               */
} rivr_pkt_hdr_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Compute CRC-16/CCITT (poly 0x1021, init 0xFFFF, no reflect) over buf.
 *
 * @param buf  Pointer to data.
 * @param len  Number of bytes.
 * @return     16-bit CRC value.
 */
uint16_t protocol_crc16(const uint8_t *buf, uint8_t len);

/**
 * @brief Encode a packet header + payload into wire format.
 *
 * The function writes:
 *   - 22-byte header (all fields, little-endian)
 *   - `payload_len` bytes from @p payload
 *   - 2-byte CRC-16 (little-endian) over the entire preceding frame
 *
 * @param hdr         Pointer to the populated header struct.
 * @param payload     Application payload bytes (may be NULL if payload_len==0).
 * @param payload_len Number of application payload bytes.
 * @param out_buf     Output buffer.
 * @param out_cap     Capacity of @p out_buf in bytes.
 *
 * @return Number of bytes written, or -1 if @p out_cap is too small.
 */
int protocol_encode(const rivr_pkt_hdr_t *hdr,
                    const uint8_t        *payload,
                    uint8_t               payload_len,
                    uint8_t              *out_buf,
                    uint8_t               out_cap);

/**
 * @brief Decode and validate a wire frame.
 *
 * Validates:
 *   - Minimum length
 *   - Magic number (RIVR_MAGIC)
 *   - CRC-16
 *
 * On success, the caller receives the decoded header and a pointer into
 * @p buf that points to the payload bytes.  The pointer is valid for as
 * long as @p buf remains in scope.
 *
 * @param buf         Wire-format frame bytes.
 * @param len         Number of bytes in @p buf.
 * @param[out] hdr    Decoded header (populated on success).
 * @param[out] payload_out  Pointer to payload bytes within @p buf.
 *
 * @return true on success (CRC OK, magic OK), false otherwise.
 */
bool protocol_decode(const uint8_t    *buf,
                     uint8_t           len,
                     rivr_pkt_hdr_t   *hdr,
                     const uint8_t   **payload_out);

#ifdef __cplusplus
}
#endif

#endif /* RIVR_PROTOCOL_H */
