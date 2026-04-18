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
 *   [17–18] seq         u16 LE  per-source application sequence (message ordering;
 *                               preserved through relay; control-plane correlation)
 *   [19–20] pkt_id      u16 LE  per-injection forwarding identity (unique per wire
 *                               injection, including retransmits and fallback floods;
 *                               dedupe keys on (src_id, pkt_id))
 *   [21]    payload_len u8      bytes of application payload following header
 *   [22]    loop_guard  u8      OR-accumulating relay fingerprint (loop defence)
 *   [23 .. 23+payload_len-1]    payload
 *   [23+payload_len .. +1]      CRC-16/CCITT (LE) over bytes [0 .. 23+payload_len-1]
 *
 * Total minimum wire size = 25 bytes (header + 0-byte payload + CRC).
 * Maximum wire size       = 25 + RIVR_PKT_MAX_PAYLOAD bytes.
 *
 * Loop-guard semantics
 * ────────────────────
 * The loop_guard field is a 1-byte OR-accumulating Bloom fingerprint that lets
 * each relay detect when a packet has circulated back to a node that already
 * forwarded it — including packets whose seq has been mutated by buggy firmware.
 *
 *   Originator sets loop_guard = 0.
 *   Each relay:
 *     1. Compute h = routing_loop_guard_hash(my_id)   (8-bit fold of node_id).
 *     2. If (loop_guard & h) == h → packet has visited this node before → drop
 *        (RIVR_FWD_DROP_LOOP, increments loop_detect_drop metric).
 *     3. Set loop_guard |= h and forward.
 *
 * The check fires when the same node receives a packet it already relayed —
 * even if seq was mutated — because its own fingerprint bits remain set in the
 * OR-accumulated byte.  False-positive probability for a legitimate 4-hop path
 * with distinct relays is < 2.5%; for a 7-hop path it is < 8%.
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
#define RIVR_PROTO_VER     1u        /**< Current protocol version           */
#define RIVR_PROTO_MIN     1u        /**< Oldest accepted protocol version   */
#define RIVR_PROTO_MAX     RIVR_PROTO_VER /**< Newest accepted protocol version   */

/* ── Packet-type constants ───────────────────────────────────────────────── */

#define PKT_CHAT           1u   /**< Text message                            */
#define PKT_BEACON         2u   /**< Periodic node-presence advertisement    */
#define PKT_ROUTE_REQ      3u   /**< Route-request (future)                  */
#define PKT_ROUTE_RPL      4u   /**< Route-reply (future)                    */
#define PKT_ACK            5u   /**< Acknowledgement (future)                */
#define PKT_DATA           6u   /**< Generic sensor data                     */
/** OTA program push: payload = null-terminated RIVR source string.
 *  Receiver stores to NVS and hot-reloads the engine.  Not relayed. */
#define PKT_PROG_PUSH      7u

/** Compact sensor / telemetry reading (SVC_TELEMETRY_PAYLOAD_LEN = 11 bytes):
 *   [0–1]  sensor_id  u16 LE — application-defined sensor identifier
 *   [2–5]  value      i32 LE — scaled integer (e.g. °C × 100, mV, ppm × 100)
 *   [6]    unit_code  u8     — UNIT_* constant (0 = NONE, 1 = CELSIUS, …)
 *   [7–10] timestamp  u32 LE — seconds since node boot (0 = unset) */
#define PKT_TELEMETRY      8u

/** Telemetry payload carried in a PKT_TELEMETRY frame.
 *  Total size = SVC_TELEMETRY_PAYLOAD_LEN (11 bytes). */
typedef struct __attribute__((packed)) {
    uint16_t sensor_id;    /**< Application-defined sensor identifier          */
    int32_t  value;        /**< Scaled integer (e.g. °C × 100, mV, ppm × 100) */
    uint8_t  unit_code;    /**< UNIT_* constant                                */
    uint32_t timestamp;    /**< Seconds since node boot (0 = unset)            */
} rivr_telemetry_payload_t;

/** Store-and-forward mailbox message (SVC_MAILBOX_HDR_LEN = 7 bytes + text):
 *   [0–3]  recipient_id  u32 LE — intended final recipient; 0 = any store node
 *   [4–5]  msg_seq       u16 LE — per-origin message sequence counter
 *   [6]    flags         u8     — MB_FLAG_* bitmask
 *   [7..N] text payload  UTF-8  — message body (N ≤ RIVR_PKT_MAX_PAYLOAD − 7) */
#define PKT_MAILBOX        9u

/** Priority event notification (SVC_ALERT_PAYLOAD_LEN = 7 bytes):
 *   [0]    severity    u8     — ALERT_SEV_INFO/WARN/CRIT (1/2/3)
 *   [1–2]  alert_code  u16 LE — application-defined event code
 *   [3–6]  value       i32 LE — associated reading (e.g. batt_mv, temp × 100) */
#define PKT_ALERT         10u

/** Compact node-metrics snapshot pushed to BLE clients every 5 s.
 *  Never relayed over LoRa (ttl=1).
 *  Two payload sizes are supported for backward compatibility:
 *    Legacy: rivr_met_ble_compact_t  (48 B)  — older firmware builds
 *    Full:   rivr_met_ble_payload_t (132 B)  — current firmware (v0.1.0+)
 *  Receivers must check payload_len to select the correct parser. */
#define PKT_METRICS       11u

/* ── Service payload lengths ─────────────────────────────────────────────── */

#define SVC_TELEMETRY_PAYLOAD_LEN  11u   /**< Fixed telemetry field size             */
#define SVC_MAILBOX_HDR_LEN         7u   /**< Fixed header in PKT_MAILBOX frames     */
/** Maximum text body in a PKT_MAILBOX payload. */
#define SVC_MAILBOX_MAX_TEXT  ((uint8_t)(RIVR_PKT_MAX_PAYLOAD - SVC_MAILBOX_HDR_LEN))
#define SVC_ALERT_PAYLOAD_LEN       7u   /**< Fixed alert field size                 */

/* ── Telemetry unit codes ────────────────────────────────────────────────── */
#define UNIT_NONE          0u   /**< Dimensionless / raw ADC                  */
#define UNIT_CELSIUS       1u   /**< Temperature — value = °C × 100          */
#define UNIT_PERCENT_RH    2u   /**< Relative humidity — value = %RH × 100   */
#define UNIT_MILLIVOLTS    3u   /**< Voltage — value = mV                     */
#define UNIT_DBM           4u   /**< Signal strength — value = dBm            */
#define UNIT_PPM           5u   /**< Gas concentration — value = ppm × 100   */
#define UNIT_CUSTOM      255u   /**< Vendor-specific; interpret via alert_code*/

/* ── Alert severity levels ───────────────────────────────────────────────── */
#define ALERT_SEV_INFO     1u   /**< Informational — no immediate action needed */
#define ALERT_SEV_WARN     2u   /**< Caution — threshold exceeded               */
#define ALERT_SEV_CRIT     3u   /**< Critical — immediate attention required    */

/* ── Mailbox flags ───────────────────────────────────────────────────────── */
#define MB_FLAG_NEW        0x01u /**< Message not yet read at destination      */
#define MB_FLAG_DELIVERED  0x02u /**< Delivery confirmed (set by recipient ACK)*/
#define MB_FLAG_FORWARD    0x04u /**< Frame is at an intermediate store node   */

/** Beacon payload layout (12 bytes after header):
 *  [0..9]  callsign (ASCII, NUL-padded)
 *  [10]    hop_count (always 0 for the originating node)
 *  [11]    role (rivr_node_role_t: 1=client, 2=repeater, 3=gateway, 0=unknown)
 */
#define BEACON_PAYLOAD_LEN    12u
#define BEACON_CALLSIGN_MAX   10u

/* ── Layout constants ────────────────────────────────────────────────────── */

/** Byte offset of the pkt_type field in the serialised wire frame. */
#define PKT_TYPE_BYTE_OFFSET   3u

/** Byte offset of the pkt_id field in the serialised wire frame. */
#define PKT_ID_BYTE_OFFSET     19u

/** Byte offset of the loop_guard field in the serialised wire frame. */
#define LOOP_GUARD_BYTE_OFFSET 22u

/** Fixed header length in bytes (before payload). */
#define RIVR_PKT_HDR_LEN       23u

/** CRC appended after payload: 2 bytes. */
#define RIVR_PKT_CRC_LEN       2u

/** Maximum application payload bytes (keeps total frame ≤ 255 bytes). */
#define RIVR_PKT_MAX_PAYLOAD   ((uint8_t)(255u - RIVR_PKT_HDR_LEN - RIVR_PKT_CRC_LEN))

/** Minimum encoded frame size (0-byte payload). */
#define RIVR_PKT_MIN_FRAME     (RIVR_PKT_HDR_LEN + RIVR_PKT_CRC_LEN)

/* ── Compile-time sanity checks ──────────────────────────────────────────── *
 * These assert that the named constants match the byte offsets documented   *
 * in the wire-layout comment at the top of this file.  A mismatch here      *
 * means either the comment or the constant has drifted — catch it at build  *
 * time rather than at a customer site.                                       *
 * ────────────────────────────────────────────────────────────────────────── */
_Static_assert(RIVR_PKT_HDR_LEN        == 23u,
               "RIVR_PKT_HDR_LEN must equal wire header byte count (23)");
_Static_assert(RIVR_PKT_MIN_FRAME      == 25u,
               "RIVR_PKT_MIN_FRAME must equal HDR_LEN(23) + CRC_LEN(2)");
_Static_assert(PKT_TYPE_BYTE_OFFSET    ==  3u,
               "pkt_type must be at wire byte offset 3");
_Static_assert(LOOP_GUARD_BYTE_OFFSET  == 22u,
               "loop_guard must be at wire byte offset 22");

/* ── Default TTL ─────────────────────────────────────────────────────────── */

#define RIVR_PKT_DEFAULT_TTL   7u   /**< Hops before a packet is discarded   */

/* ── Flags bitmask ───────────────────────────────────────────────────────── */

#define PKT_FLAG_ACK_REQ   0x01u   /**< Sender requests an ACK              */
#define PKT_FLAG_RELAY     0x02u   /**< Packet has been relayed              */
#define PKT_FLAG_FALLBACK  0x04u   /**< Unicast failed; re-sent as flood     */

/**
 * PKT_FLAG_CHANNEL (0x08) — channel_id prefix present in PKT_CHAT payload.
 *
 * When this flag is set on a PKT_CHAT frame the first two bytes of the payload
 * carry the channel_id as a u16 little-endian value; the UTF-8 message text
 * follows immediately at payload[2].
 *
 *   payload[0..1]  channel_id  u16 LE   (RIVR_CHAN_GLOBAL = 0 for public chat)
 *   payload[2..]   text        UTF-8    (NUL not required — use payload_len)
 *
 * When this flag is CLEAR the payload is raw UTF-8 (v1 legacy behaviour);
 * receivers should treat such frames as belonging to channel 0 (Global).
 *
 * RELAY COMPATIBILITY
 * ───────────────────
 * PKT_FLAG_CHANNEL is transparent to v1 relay nodes.  A v1 node relays ALL
 * PKT_CHAT frames that pass magic + CRC validation, regardless of flags bits.
 * The payload integrity is protected by the existing CRC-16, so no relay node
 * can corrupt the channel_id silently.
 *
 * MINIMUM PAYLOAD LENGTH when PKT_FLAG_CHANNEL is set: 3 bytes
 *   (2 channel header + at least 1 byte of text).
 */
#define PKT_FLAG_CHANNEL   0x08u

/**
 * PKT_FLAG_HAS_POS (0x10) — lat/lon appended to PKT_BEACON payload.
 *
 * When set on a PKT_BEACON frame the payload is 20 bytes instead of 12:
 *   [0..9]   callsign  ASCII NUL-padded
 *   [10]     hop_count 0 at origin
 *   [11]     role      rivr_node_role_t
 *   [12..15] lat_e7    i32 LE  degrees × 1e7  (INT32_MIN = unknown)
 *   [16..19] lon_e7    i32 LE  degrees × 1e7  (INT32_MIN = unknown)
 *
 * Backward-compatible: nodes without this firmware bit decode at most
 * BEACON_PAYLOAD_LEN bytes and ignore the extra 8 bytes harmlessly.
 */
#define PKT_FLAG_HAS_POS   0x10u

/** Extended beacon payload length when PKT_FLAG_HAS_POS is set (20 bytes). */
#define BEACON_PAYLOAD_LEN_POS  20u

/** Byte length of the channel_id prefix when PKT_FLAG_CHANNEL is set. */
#define RIVR_CHAT_CHAN_HDR_LEN  2u

/* ── Fallback-flood TTL ─────────────────────────────────────────────────── *
 * When a unicast TX attempt fails (queue full or route stale), the sender   *
 * re-encodes the frame as broadcast with this reduced TTL so it does not    *
 * propagate as far as a normal flood.                                       *
 * ─────────────────────────────────────────────────────────────────────────── */
#define RIVR_FALLBACK_TTL  3u   /**< TTL for unicast-failed flood fallback   */

/**
 * ACK payload layout (ACK_PAYLOAD_LEN = 6 bytes):
 *   [0–3]  ack_src_id  u32 LE — src_id of the frame being acknowledged
 *   [4–5]  ack_pkt_id  u16 LE — pkt_id currently active in sender's retry entry
 */
#define ACK_PAYLOAD_LEN  6u

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
    uint16_t seq;          /**< Application/source sequence: per-source      *
                            *   message counter, preserved through relay.    *
                            *   Used for: application ordering, control-plane*
                            *   correlation (ROUTE_REQ/RPL, future ACK).     */
    uint16_t pkt_id;       /**< Per-injection forwarding identity: unique per *
                            *   wire injection (new for each retransmit,     *
                            *   fallback flood, or control-plane frame).     *
                            *   Dedupe keys on (src_id, pkt_id); jitter      *
                            *   seeds from pkt_id.  Originator increments a *
                            *   monotonic counter (g_ctrl_seq) per injection.*/
    uint8_t  payload_len;  /**< Length of accompanying payload               */
    uint8_t  loop_guard;   /**< OR-accumulating relay fingerprint (loop def) */
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
