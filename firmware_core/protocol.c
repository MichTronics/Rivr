/**
 * @file  protocol.c
 * @brief RIVR binary on-air packet encode / decode + CRC-16/CCITT.
 */

#include "protocol.h"
#include <string.h>

/* ── CRC-16/CCITT ─────────────────────────────────────────────────────────── *
 *
 *  Poly: 0x1021  Init: 0xFFFF  RefIn: false  RefOut: false  XorOut: 0x0000
 *  This matches the "ISO HDLC / X.25 / CCITT CRC-16" variant often used in
 *  radio protocols.  Table-driven for speed; <512 bytes of ROM cost.
 * ────────────────────────────────────────────────────────────────────────── */

static const uint16_t s_crc16_table[256] = {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50a5,0x60c6,0x70e7,
    0x8108,0x9129,0xa14a,0xb16b,0xc18c,0xd1ad,0xe1ce,0xf1ef,
    0x1231,0x0210,0x3273,0x2252,0x52b5,0x4294,0x72f7,0x62d6,
    0x9339,0x8318,0xb37b,0xa35a,0xd3bd,0xc39c,0xf3ff,0xe3de,
    0x2462,0x3443,0x0420,0x1401,0x64e6,0x74c7,0x44a4,0x5485,
    0xa56a,0xb54b,0x8528,0x9509,0xe5ee,0xf5cf,0xc5ac,0xd58d,
    0x3653,0x2672,0x1611,0x0630,0x76d7,0x66f6,0x5695,0x46b4,
    0xb75b,0xa77a,0x9719,0x8738,0xf7df,0xe7fe,0xd79d,0xc7bc,
    0x48c4,0x58e5,0x6886,0x78a7,0x0840,0x1861,0x2802,0x3823,
    0xc9cc,0xd9ed,0xe98e,0xf9af,0x8948,0x9969,0xa90a,0xb92b,
    0x5af5,0x4ad4,0x7ab7,0x6a96,0x1a71,0x0a50,0x3a33,0x2a12,
    0xdbfd,0xcbdc,0xfbbf,0xeb9e,0x9b79,0x8b58,0xbb3b,0xab1a,
    0x6ca6,0x7c87,0x4ce4,0x5cc5,0x2c22,0x3c03,0x0c60,0x1c41,
    0xedae,0xfd8f,0xcdec,0xddcd,0xad2a,0xbd0b,0x8d68,0x9d49,
    0x7e97,0x6eb6,0x5ed5,0x4ef4,0x3e13,0x2e32,0x1e51,0x0e70,
    0xff9f,0xefbe,0xdfdd,0xcffc,0xbf1b,0xaf3a,0x9f59,0x8f78,
    0x9188,0x81a9,0xb1ca,0xa1eb,0xd10c,0xc12d,0xf14e,0xe16f,
    0x1080,0x00a1,0x30c2,0x20e3,0x5004,0x4025,0x7046,0x6067,
    0x83b9,0x9398,0xa3fb,0xb3da,0xc33d,0xd31c,0xe37f,0xf35e,
    0x02b1,0x1290,0x22f3,0x32d2,0x4235,0x5214,0x6277,0x7256,
    0xb5ea,0xa5cb,0x95a8,0x8589,0xf56e,0xe54f,0xd52c,0xc50d,
    0x34e2,0x24c3,0x14a0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xa7db,0xb7fa,0x8799,0x97b8,0xe75f,0xf77e,0xc71d,0xd73c,
    0x26d3,0x36f2,0x0691,0x16b0,0x6657,0x7676,0x4615,0x5634,
    0xd94c,0xc96d,0xf90e,0xe92f,0x99c8,0x89e9,0xb98a,0xa9ab,
    0x5844,0x4865,0x7806,0x6827,0x18c0,0x08e1,0x3882,0x28a3,
    0xcb7d,0xdb5c,0xeb3f,0xfb1e,0x8bf9,0x9bd8,0xabbb,0xbb9a,
    0x4a75,0x5a54,0x6a37,0x7a16,0x0af1,0x1ad0,0x2ab3,0x3a92,
    0xfd2e,0xed0f,0xdd6c,0xcd4d,0xbdaa,0xad8b,0x9de8,0x8dc9,
    0x7c26,0x6c07,0x5c64,0x4c45,0x3ca2,0x2c83,0x1ce0,0x0cc1,
    0xef1f,0xff3e,0xcf5d,0xdf7c,0xaf9b,0xbfba,0x8fd9,0x9ff8,
    0x6e17,0x7e36,0x4e55,0x5e74,0x2e93,0x3eb2,0x0ed1,0x1ef0,
};

uint16_t protocol_crc16(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc >> 8) ^ buf[i]);
        crc = (uint16_t)((crc << 8) ^ s_crc16_table[idx]);
    }
    return crc;
}

/* ── encode ──────────────────────────────────────────────────────────────── */

int protocol_encode(const rivr_pkt_hdr_t *hdr,
                    const uint8_t        *payload,
                    uint8_t               payload_len,
                    uint8_t              *out_buf,
                    uint8_t               out_cap)
{
    if (!hdr || !out_buf) return -1;

    uint8_t total = (uint8_t)(RIVR_PKT_HDR_LEN + payload_len + RIVR_PKT_CRC_LEN);
    if (total > out_cap) return -1;
    if (payload_len > RIVR_PKT_MAX_PAYLOAD) return -1;

    uint8_t *p = out_buf;

    /* [0–1] magic LE */
    p[0] = (uint8_t)(RIVR_MAGIC & 0xFFu);
    p[1] = (uint8_t)(RIVR_MAGIC >> 8);
    /* [2] version */
    p[2] = RIVR_PROTO_VER;
    /* [3] pkt_type */
    p[3] = hdr->pkt_type;
    /* [4] flags */
    p[4] = hdr->flags;
    /* [5] ttl */
    p[5] = hdr->ttl;
    /* [6] hop */
    p[6] = hdr->hop;
    /* [7–8] net_id LE */
    p[7] = (uint8_t)(hdr->net_id & 0xFFu);
    p[8] = (uint8_t)(hdr->net_id >> 8);
    /* [9–12] src_id LE */
    p[9]  = (uint8_t)(hdr->src_id        & 0xFFu);
    p[10] = (uint8_t)((hdr->src_id >>  8) & 0xFFu);
    p[11] = (uint8_t)((hdr->src_id >> 16) & 0xFFu);
    p[12] = (uint8_t)((hdr->src_id >> 24) & 0xFFu);
    /* [13–16] dst_id LE */
    p[13] = (uint8_t)(hdr->dst_id        & 0xFFu);
    p[14] = (uint8_t)((hdr->dst_id >>  8) & 0xFFu);
    p[15] = (uint8_t)((hdr->dst_id >> 16) & 0xFFu);
    p[16] = (uint8_t)((hdr->dst_id >> 24) & 0xFFu);
    /* [17–20] seq LE */
    p[17] = (uint8_t)(hdr->seq        & 0xFFu);
    p[18] = (uint8_t)((hdr->seq >>  8) & 0xFFu);
    p[19] = (uint8_t)((hdr->seq >> 16) & 0xFFu);
    p[20] = (uint8_t)((hdr->seq >> 24) & 0xFFu);
    /* [21] payload_len */
    p[21] = payload_len;
    /* [22] loop_guard */
    p[22] = hdr->loop_guard;

    /* [23 .. 23+payload_len-1] payload */
    if (payload_len > 0 && payload) {
        memcpy(p + 23, payload, payload_len);
    }

    /* CRC over header + payload (bytes 0 .. 22+payload_len) */
    uint8_t  data_len = (uint8_t)(RIVR_PKT_HDR_LEN + payload_len);
    uint16_t crc      = protocol_crc16(out_buf, data_len);
    p[23 + payload_len]     = (uint8_t)(crc & 0xFFu);
    p[23 + payload_len + 1] = (uint8_t)(crc >> 8);

    return (int)total;
}

/* ── decode ──────────────────────────────────────────────────────────────── */

bool protocol_decode(const uint8_t    *buf,
                     uint8_t           len,
                     rivr_pkt_hdr_t   *hdr,
                     const uint8_t   **payload_out)
{
    if (!buf || !hdr) return false;

    /* Minimum: header (22) + CRC (2) */
    if (len < RIVR_PKT_MIN_FRAME) return false;

    /* Check magic */
    uint16_t magic = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    if (magic != RIVR_MAGIC) return false;

    /* Extract payload_len from header byte [21] */
    uint8_t payload_len = buf[21];

    /* Check total length */
    uint8_t expected_len = (uint8_t)(RIVR_PKT_HDR_LEN + payload_len + RIVR_PKT_CRC_LEN);
    if (len < expected_len) return false;

    /* Verify CRC (covers bytes 0 .. 21+payload_len) */
    uint8_t  data_len   = (uint8_t)(RIVR_PKT_HDR_LEN + payload_len);
    uint16_t crc_calc   = protocol_crc16(buf, data_len);
    uint16_t crc_wire   = (uint16_t)(buf[data_len] | ((uint16_t)buf[data_len + 1] << 8));
    if (crc_calc != crc_wire) return false;

    /* Populate header struct */
    hdr->magic       = magic;
    hdr->version     = buf[2];
    hdr->pkt_type    = buf[3];
    hdr->flags       = buf[4];
    hdr->ttl         = buf[5];
    hdr->hop         = buf[6];
    hdr->net_id      = (uint16_t)(buf[7]  | ((uint16_t)buf[8]  << 8));
    hdr->src_id      = (uint32_t)(buf[9]  | ((uint32_t)buf[10] << 8)
                                           | ((uint32_t)buf[11] << 16)
                                           | ((uint32_t)buf[12] << 24));
    hdr->dst_id      = (uint32_t)(buf[13] | ((uint32_t)buf[14] << 8)
                                           | ((uint32_t)buf[15] << 16)
                                           | ((uint32_t)buf[16] << 24));
    hdr->seq         = (uint32_t)(buf[17] | ((uint32_t)buf[18] << 8)
                                           | ((uint32_t)buf[19] << 16)
                                           | ((uint32_t)buf[20] << 24));
    hdr->payload_len = payload_len;
    hdr->loop_guard  = buf[LOOP_GUARD_BYTE_OFFSET];   /* byte [22] */

    /* Pointer into @p buf for the payload */
    if (payload_out) {
        *payload_out = (payload_len > 0) ? (buf + RIVR_PKT_HDR_LEN) : NULL;
    }

    return true;
}
