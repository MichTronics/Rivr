/**
 * @file  rivr_decode.c
 * @brief RIVR wire-frame decoder — reads hex or binary frames and writes JSONL.
 *
 * Usage (build):
 *   gcc -O2 -std=c11 -I../firmware_core -I.. \
 *       ../firmware_core/protocol.c rivr_decode.c \
 *       -o rivr_decode
 *
 *   # Or from the tools/ directory relative to the project root:
 *   make -C tools
 *
 * Usage (run):
 *   # Hex input from stdin (space or newline separated, case-insensitive):
 *   echo "52 56 01 01 00 07 00 00 00 ..." | ./rivr_decode
 *
 *   # Binary file argument:
 *   ./rivr_decode capture.bin
 *
 *   # Pipe from socat / lora-reader:
 *   socat - /dev/ttyUSB0,b115200 | xxd -p | tr -d '\n' | ./rivr_decode
 *
 * Output (one JSON object per frame, newline-delimited):
 *   {"ok":true,"magic":"5256","version":1,"pkt_type":1,"type":"CHAT",
 *    "flags":0,"ttl":7,"hop":0,"net_id":0,"src_id":1,"dst_id":0,
 *    "seq":1,"payload_len":5,"payload_hex":"48656c6c6f","crc_ok":true}
 *
 *   {"ok":false,"error":"short frame","raw_hex":"5256010102"}
 *
 * Exit codes:
 *   0  All frames decoded successfully (crc_ok may be false for bad CRC).
 *   1  Fatal error (file not found, read error, etc.).
 *
 * Build dependencies:
 *   - firmware_core/protocol.h  (header struct, constants)
 *   - firmware_core/protocol.c  (protocol_decode, protocol_crc16)
 *   No ESP-IDF, no FreeRTOS, no dynamic allocation required.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "protocol.h"
#include "rivr_ota.h"

/* ── PKT type name lookup ─────────────────────────────────────────────────── */

static const char *pkt_type_name(uint8_t t)
{
    switch (t) {
        case 1:  return "CHAT";
        case 2:  return "BEACON";
        case 3:  return "ROUTE_REQ";
        case 4:  return "ROUTE_RPL";
        case 5:  return "ACK";
        case 6:  return "DATA";
        case 7:  return "PROG_PUSH";
        default: return "UNKNOWN";
    }
}

/* ── Hex helpers ─────────────────────────────────────────────────────────── */

static void bytes_to_hex(const uint8_t *buf, size_t len, char *out, size_t out_cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 <= out_cap; i++) {
        pos += (size_t)snprintf(out + pos, out_cap - pos, "%02x", (unsigned)buf[i]);
    }
    out[pos < out_cap ? pos : out_cap - 1] = '\0';
}

/* ── JSON output ─────────────────────────────────────────────────────────── */

static void emit_error(const uint8_t *raw, size_t raw_len, const char *error)
{
    /* Emit a "hex dump" of the raw bytes that failed */
    char hexbuf[512];
    bytes_to_hex(raw, raw_len < 200 ? raw_len : 200, hexbuf, sizeof(hexbuf));
    printf("{\"ok\":false,\"error\":\"%s\",\"raw_hex\":\"%s\"}\n", error, hexbuf);
}

static void emit_frame(const uint8_t           *buf,
                        uint8_t                  frame_len,
                        const rivr_pkt_hdr_t    *hdr,
                        const uint8_t           *payload,
                        bool                     crc_ok)
{
    char payload_hex[512] = "";
    if (hdr->payload_len > 0 && payload) {
        bytes_to_hex(payload, hdr->payload_len, payload_hex, sizeof(payload_hex));
    }

    /* Extra beacon info */
    char extra[256] = "";
    if (hdr->pkt_type == PKT_BEACON && hdr->payload_len >= BEACON_PAYLOAD_LEN) {
        /* callsign: up to BEACON_CALLSIGN_MAX printable ASCII chars */
        char callsign[BEACON_CALLSIGN_MAX + 1];
        memcpy(callsign, payload, BEACON_CALLSIGN_MAX);
        callsign[BEACON_CALLSIGN_MAX] = '\0';
        /* trim trailing NUL */
        for (int i = BEACON_CALLSIGN_MAX - 1; i >= 0 && callsign[i] == '\0'; i--)
            callsign[i] = '\0';
        uint8_t hop_count = payload[10];
        snprintf(extra, sizeof(extra),
                 ",\"beacon\":{\"callsign\":\"%s\",\"hop_count\":%u}",
                 callsign, (unsigned)hop_count);
    } else if (hdr->pkt_type == PKT_PROG_PUSH &&
               hdr->payload_len > RIVR_OTA_HDR_LEN) {
        /* OTA push: show key_id and seq */
        uint8_t  key_id  = payload[64];
        uint32_t ota_seq = (uint32_t)payload[65]
                         | ((uint32_t)payload[66] << 8)
                         | ((uint32_t)payload[67] << 16)
                         | ((uint32_t)payload[68] << 24);
        /* program text: payload + RIVR_OTA_HDR_LEN */
        size_t text_len = (size_t)(hdr->payload_len) - RIVR_OTA_HDR_LEN;
        char text_preview[64] = "";
        size_t copy = text_len < sizeof(text_preview) - 1
                    ? text_len : sizeof(text_preview) - 1;
        memcpy(text_preview, payload + RIVR_OTA_HDR_LEN, copy);
        text_preview[copy] = '\0';
        /* Escape quotes in text_preview conservatively */
        for (size_t i = 0; i < copy; i++) {
            if ((uint8_t)text_preview[i] < 0x20u || text_preview[i] == '"')
                text_preview[i] = '?';
        }
        snprintf(extra, sizeof(extra),
                 ",\"ota\":{\"key_id\":%u,\"seq\":%u,\"text_preview\":\"%s\"}",
                 (unsigned)key_id, (unsigned)ota_seq, text_preview);
    }

    printf("{\"ok\":true"
           ",\"magic\":\"%04x\""
           ",\"version\":%u"
           ",\"pkt_type\":%u,\"type\":\"%s\""
           ",\"flags\":%u"
           ",\"ttl\":%u,\"hop\":%u"
           ",\"net_id\":%u"
           ",\"src_id\":%u,\"dst_id\":%u"
           ",\"seq\":%u"
           ",\"payload_len\":%u,\"payload_hex\":\"%s\""
           ",\"crc_ok\":%s"
           "%s"
           "}\n",
           (unsigned)hdr->magic,
           (unsigned)hdr->version,
           (unsigned)hdr->pkt_type, pkt_type_name(hdr->pkt_type),
           (unsigned)hdr->flags,
           (unsigned)hdr->ttl, (unsigned)hdr->hop,
           (unsigned)hdr->net_id,
           (unsigned)hdr->src_id, (unsigned)hdr->dst_id,
           (unsigned)hdr->seq,
           (unsigned)hdr->payload_len, payload_hex,
           crc_ok ? "true" : "false",
           extra);
    (void)buf; (void)frame_len;
}

/* ── Single frame decode ──────────────────────────────────────────────────── */

static void decode_frame(const uint8_t *buf, size_t len)
{
    if (len == 0) return;

    if (len > 255) {
        emit_error(buf, 255, "frame too long (>255 bytes)");
        return;
    }

    uint8_t frame_len = (uint8_t)len;

    if (frame_len < RIVR_PKT_MIN_FRAME) {
        emit_error(buf, frame_len, "short frame");
        return;
    }

    /* Check magic without calling full decode, for better error messages */
    uint16_t magic = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (magic != RIVR_MAGIC) {
        emit_error(buf, frame_len, "bad magic");
        return;
    }

    rivr_pkt_hdr_t   hdr;
    const uint8_t   *payload = NULL;

    /* protocol_decode validates CRC and magic */
    bool crc_ok = protocol_decode(buf, frame_len, &hdr, &payload);
    emit_frame(buf, frame_len, &hdr, payload, crc_ok);
}

/* ── Hex input reader ─────────────────────────────────────────────────────── */
/*
 * Read whitespace-separated hex bytes from fp.
 * Supports both "52 56 01" (spaced) and "525601" (dense) formats.
 * Returns number of bytes read, or (size_t)-1 on read error.
 */
static size_t read_hex(FILE *fp, uint8_t *buf, size_t cap)
{
    size_t  n    = 0;
    int     hi   = EOF;
    int     c;

    while ((c = fgetc(fp)) != EOF) {
        if (isspace((unsigned char)c)) {
            /* Flush pending nibble if any */
            if (hi != EOF) {
                if (n >= cap) break;
                buf[n++] = (uint8_t)(hi << 4);
                hi = EOF;
            }
            continue;
        }
        int nib;
        if (c >= '0' && c <= '9')      nib = c - '0';
        else if (c >= 'a' && c <= 'f') nib = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nib = c - 'A' + 10;
        else continue; /* skip non-hex chars silently */

        if (hi == EOF) {
            hi = nib;
        } else {
            if (n >= cap) break;
            buf[n++] = (uint8_t)((hi << 4) | nib);
            hi = EOF;
        }
    }
    /* Flush any trailing nibble */
    if (hi != EOF && n < cap) {
        buf[n++] = (uint8_t)(hi << 4);
    }
    return n;
}

/* ── Binary multi-frame reader ───────────────────────────────────────────── */
/*
 * In binary mode the stream is a concatenation of self-delimiting frames.
 * Each frame carries its own payload_len at offset 21, so we can calculate
 * exact frame size = RIVR_PKT_HDR_LEN + payload_len + RIVR_PKT_CRC_LEN.
 *
 * We read one frame at a time, decode it, then continue.
 */
static void decode_binary_stream(FILE *fp)
{
    uint8_t buf[255];

    for (;;) {
        /* Read minimum header to determine full frame size */
        size_t hdr_bytes = fread(buf, 1, RIVR_PKT_HDR_LEN, fp);
        if (hdr_bytes == 0) break;  /* clean EOF */
        if (hdr_bytes < RIVR_PKT_HDR_LEN) {
            emit_error(buf, hdr_bytes, "truncated header in stream");
            break;
        }

        /* Extract payload_len from wire bytes (offset 21) */
        uint8_t payload_len = buf[21];
        size_t  frame_len   = RIVR_PKT_HDR_LEN + (size_t)payload_len + RIVR_PKT_CRC_LEN;

        if (frame_len > sizeof(buf)) {
            emit_error(buf, RIVR_PKT_HDR_LEN, "payload_len too large");
            break;
        }

        /* Read the rest (payload + CRC) */
        size_t rest = fread(buf + RIVR_PKT_HDR_LEN, 1,
                            payload_len + RIVR_PKT_CRC_LEN, fp);
        if (rest < (size_t)(payload_len + RIVR_PKT_CRC_LEN)) {
            emit_error(buf, RIVR_PKT_HDR_LEN + rest, "truncated payload in stream");
            break;
        }

        decode_frame(buf, frame_len);
    }
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [frame.bin]\n", argv[0]);
        fprintf(stderr, "       echo 'hex bytes' | %s\n", argv[0]);
        return 1;
    }

    if (argc == 2) {
        /* Binary file mode: open and stream-decode all frames */
        FILE *fp = fopen(argv[1], "rb");
        if (!fp) {
            fprintf(stderr, "%s: cannot open '%s': %s\n",
                    argv[0], argv[1], strerror(errno));
            return 1;
        }
        decode_binary_stream(fp);
        fclose(fp);
    } else {
        /* Hex stdin mode: read all hex bytes, decode as a single frame */
        uint8_t buf[255];
        size_t  len = read_hex(stdin, buf, sizeof(buf));
        if (len == 0) {
            fprintf(stderr, "%s: no input\n", argv[0]);
            return 1;
        }
        decode_frame(buf, len);
    }

    return 0;
}


