# RIVR Protocol Reference

> **Version:** 1 (current)  
> **Source of truth:** `firmware_core/protocol.h`, `firmware_core/rivr_ota.h`  
> **CRC polynomial:** CRC-16/CCITT (poly 0x1021, init 0xFFFF, no reflection)

---

## 1. On-Air Frame Layout

Every RIVR packet is encoded as a single contiguous byte buffer transmitted
over LoRa at the physical layer. Byte order is **little-endian** throughout.

```
┌──────────────────────────────────────────────────────────────┐
│  Offset │ Size │ Field       │ Description                    │
├─────────┼──────┼─────────────┼────────────────────────────────┤
│    0    │  2   │ magic       │ 0x5256 LE = "RV"               │
│    2    │  1   │ version     │ Protocol version (= 1)         │
│    3    │  1   │ pkt_type    │ PKT_* constant (see §3)        │
│    4    │  1   │ flags       │ PKT_FLAG_* bitmask (see §4)    │
│    5    │  1   │ ttl         │ Hops remaining (default 7)     │
│    6    │  1   │ hop         │ Hops taken (incremented on fwd)│
│    7    │  2   │ net_id      │ Network / channel discriminator│
│    9    │  4   │ src_id      │ Sender node unique ID          │
│   13    │  4   │ dst_id      │ Destination ID (0 = broadcast) │
│   17    │  2   │ seq         │ Per-source application counter │
│   19    │  2   │ pkt_id      │ Per-injection forwarding ID    │
│   21    │  1   │ payload_len │ Bytes of application payload   │
│   22    │  1   │ loop_guard  │ OR-accumulating relay fingerprint│
│   23    │  N   │ payload     │ Application payload (0–230 B)  │
│  23+N   │  2   │ crc16       │ CRC-16/CCITT over bytes[0..23+N-1]│
└──────────────────────────────────────────────────────────────┘
```

- **Minimum frame size:** 25 bytes (`RIVR_PKT_MIN_FRAME` = 23 + 0 + 2)
- **Maximum frame size:** 255 bytes (LoRa payload limit)
- **Maximum payload:** 230 bytes (`RIVR_PKT_MAX_PAYLOAD` = 255 − 23 − 2)

### CRC Computation

```
CRC-16/CCITT: poly=0x1021, init=0xFFFF, input/output not reflected
Covers: all bytes from offset 0 through offset 23+N−1 (inclusive)
Appended as two bytes, little-endian, starting at offset 23+N
```

---

## 2. Field Details

### `magic` (bytes 0–1, u16 LE)

Always `0x5256` (ASCII "RV"). A packet with any other magic value is silently
discarded by all RIVR nodes.

### `version` (byte 2, u8)

Protocol version. Currently `1`. Packets with an unrecognised version are
silently discarded.

### `pkt_type` (byte 3, u8)

Identifies the payload format. See §3.

### `flags` (byte 4, u8)

Bitmask of per-packet flags. See §4.

### `ttl` (byte 5, u8)

Hop budget. Set to `RIVR_PKT_DEFAULT_TTL` (7) by the originating node.
Decremented by each relay before re-transmitting. A packet with `ttl == 0`
on arrival is dropped and not forwarded.

### `hop` (byte 6, u8)

Hop count. Set to 0 by the originating node and incremented by each relay.
Mainly diagnostic; allows receiving nodes to estimate path length.

### `net_id` (bytes 7–8, u16 LE)

Network / channel discriminator. All nodes in the same logical network must
share the same `net_id`. Packets with a mismatched `net_id` are discarded
before forwarding.

### `src_id` (bytes 9–12, u32 LE)

Sender node unique identifier (e.g. derived from chip MAC). A node never
forwards its own packets.

### `dst_id` (bytes 13–16, u32 LE)

Destination node ID. `0x00000000` means **broadcast** (all nodes accept and
potentially relay). A non-zero value targets a specific node; other nodes
may still relay the packet but will not pass it to the application layer.

### `seq` (bytes 17–18, u16 LE)

Per-source application sequence counter. Preserved through relay hops.
Used for application-level message ordering and control-plane correlation.
**Not** used for deduplication — see `pkt_id` below.

### `pkt_id` (bytes 19–20, u16 LE)

Per-injection forwarding identity. Unique per wire injection, including
retransmits and fallback floods. The dedupe cache keys on `(src_id, pkt_id)`
to suppress duplicates within `DEDUPE_EXPIRY_MS` (60 s) of first arrival.
Jitter delay is also seeded from `pkt_id`.

### `loop_guard` (byte 22, u8)

OR-accumulating relay fingerprint. Set to 0 by the originator. Each relay
computes an 8-bit fold of its own `node_id` and checks whether those bits
are already set in `loop_guard`; if so the packet has already visited this
node and is dropped (`loop_detect_drop` metric). Otherwise the relay sets
those bits before forwarding. False-positive probability is < 2.5% for a
4-hop path with distinct relays.

### `payload_len` (byte 21, u8)

Number of application payload bytes immediately following the header. Valid
range: 0–230. A `payload_len` value that would place the CRC beyond the
received frame length causes the packet to be discarded.

---

## 3. Packet Types

| Constant       | Value | Description                               | Payload format |
|----------------|-------|-------------------------------------------|----------------|
| `PKT_CHAT`     | 1     | Plain-text chat message                   | UTF-8 text (no NUL required) |
| `PKT_BEACON`   | 2     | Periodic node-presence advertisement      | See §5.1 |
| `PKT_ROUTE_REQ`| 3     | Route-request                             | See §5.3 |
| `PKT_ROUTE_RPL`| 4     | Route-reply                               | See §5.3 |
| `PKT_ACK`      | 5     | Acknowledgement                           | See §5.4 |
| `PKT_DATA`     | 6     | Generic sensor data (application-defined) | Application-specific |
| `PKT_PROG_PUSH`| 7     | OTA RIVR program push                     | See §5.2 |
| `PKT_TELEMETRY`| 8     | Structured sensor reading                 | 11 bytes fixed — see [services.md](services.md#4-telemetry-pkt_telemetry--8) |
| `PKT_MAILBOX`  | 9     | Store-and-forward message                 | 7-byte header + UTF-8 body — see [services.md](services.md#5-mailbox-pkt_mailbox--9) |
| `PKT_ALERT`    | 10    | Priority event notification               | 7 bytes fixed — see [services.md](services.md#6-alert-pkt_alert--10) |
| `PKT_METRICS`  | 11    | Firmware diagnostics snapshot             | 132-byte `rivr_met_ble_payload_t`; BLE-only (TTL=1, never LoRa-relayed) |

Packet type 0 and values 12–255 are reserved. Unrecognised types are forwarded
unchanged (relay nodes do not inspect payload beyond the common header).

---

## 4. Flags Bitmask

| Bit mask | Constant           | Set by       | Meaning |
|----------|--------------------|--------------|---------|
| `0x01`   | `PKT_FLAG_ACK_REQ` | Originator   | Receiver should send a `PKT_ACK` unicast |
| `0x02`   | `PKT_FLAG_RELAY`   | Relay node   | Packet has been forwarded at least once |
| `0x04`   | `PKT_FLAG_FALLBACK`| Relay node   | Unicast delivery failed; re-sent as flood |
| `0xF8`   | *(reserved)*       | —            | Must be 0 on transmit; ignored on receive |

---

## 5. Payload Formats

### 5.1 Beacon Payload (`PKT_BEACON`)

Fixed 11-byte payload. Always set `payload_len = 11`.

```
┌─────────┬──────┬────────────────────────────────────┐
│  Offset │ Size │ Field                               │
├─────────┼──────┼────────────────────────────────────┤
│    0    │  10  │ callsign (ASCII, NUL-padded)        │
│   10    │   1  │ hop_count (0 for originating node) │
└─────────┴──────┴────────────────────────────────────┘
```

- `callsign` is a human-readable identifier (e.g. amateur radio callsign or
  node name). Right-padded with NUL (`0x00`). Not NUL-terminated in the
  wire format—the full 10 bytes are always transmitted.
- `hop_count` is always 0 for the beacon originator. Relay nodes copy the
  hop field from the enclosing packet header; this field is application-level
  diagnostic data only.

### 5.2 OTA Program Push Payload (`PKT_PROG_PUSH`)

Carries a signed RIVR program. The receiver validates the signature and
anti-replay counter before storing the program in NVS.

```
┌─────────┬──────┬────────────────────────────────────────────────┐
│  Offset │ Size │ Field                                           │
├─────────┼──────┼────────────────────────────────────────────────┤
│    0    │  64  │ Ed25519 signature (detached)                    │
│   64    │   1  │ key_id  (u8, selects key from RIVR_OTA_KEYS[]) │
│   65    │   4  │ seq     (u32 LE, monotonically increasing)      │
│   69    │   N  │ RIVR program text (UTF-8, NUL-terminated in NVS│
└─────────┴──────┴────────────────────────────────────────────────┘
```

**Total header overhead:** 69 bytes (`RIVR_OTA_HDR_LEN`)
**Maximum program text:** 161 bytes (`RIVR_PKT_MAX_PAYLOAD` − `RIVR_OTA_HDR_LEN` = 230 − 69).

**Signature coverage:** `sig` covers the concatenation `(key_id ‖ seq ‖ program_text)`,
i.e. all bytes from offset 64 to end of payload. The `key_id` byte is inside
the signature so it cannot be rewritten without invalidating the signature.

**Key ring:** Up to `RIVR_OTA_KEY_COUNT` (currently 2) public keys are
embedded in firmware. `key_id == 0` is the primary key; `key_id == 1` is
the rotation/secondary key. Out-of-range `key_id` values are rejected
before any cryptographic work.

**Anti-replay:** The `seq` value must be **strictly greater** than the last
accepted `seq` stored in NVS. The firmware persists the new `seq` after a
successful verify so reboots do not open replay windows.

**Boot-confirm:** After `rivr_ota_activate()` the firmware sets an
`ota_pending` NVS flag. The running RIVR program must call
`rivr_ota_confirm()` within the session; if the device reboots while
`ota_pending == 1` the platform layer may roll back to the previous program.

### 5.3 Chat Payload (`PKT_CHAT`)

Arbitrary UTF-8 text up to `payload_len` bytes. No NUL terminator is
required in the wire payload; the application layer should NUL-terminate
before display.

### 5.4 Data Payload (`PKT_DATA`)

Application-defined. The RIVR runtime does not interpret this payload; it
is delivered verbatim to registered data-sink callbacks.

---

## 6. Routing Behaviour

### 6.1 Flood Forwarding

1. **Dedupe check** — if `(src_id, pkt_id)` in the local cache and not expired
   (< 60 s), drop silently (`RIVR_FWD_DROP_DEDUPE`).
2. **TTL check** — if `ttl == 0` on arrival, drop (`RIVR_FWD_DROP_TTL`).
3. **Forward budget** — if the per-type airtime or rate budget would be
   exceeded, drop (`RIVR_FWD_DROP_BUDGET`).
4. **Mutate** — decrement `ttl`, increment `hop`, set `PKT_FLAG_RELAY`.
5. **Re-encode** — recompute CRC over the modified header + payload.
6. **Jitter delay** — wait a deterministic `routing_jitter_ticks()` interval
   (0–200 ms) before transmitting to reduce collision probability.

Nodes never forward their own `src_id` packets.

### 6.2 Net-ID Filtering

Packets with a `net_id` not matching the local node's configured `net_id`
are discarded at the radio receive callback, before any routing logic runs.

### 6.3 Duplicate Cache

The dedupe cache is a power-of-2 ring buffer (`DEDUPE_CACHE_SIZE = 32`)
keyed on `(src_id, pkt_id)`. Entries expire after `DEDUPE_EXPIRY_MS = 60000` ms
to allow legitimate retransmissions after long silences.

---

## 7. Constants Quick Reference

| Constant                | Value | Source file        |
|-------------------------|-------|--------------------|
| `RIVR_MAGIC`            | 0x5256 | `protocol.h`     |
| `RIVR_PROTO_VER`        | 1      | `protocol.h`     |
| `RIVR_PKT_HDR_LEN`      | 23     | `protocol.h`     |
| `RIVR_PKT_CRC_LEN`      | 2      | `protocol.h`     |
| `RIVR_PKT_MIN_FRAME`    | 25     | `protocol.h`     |
| `RIVR_PKT_MAX_PAYLOAD`  | 230    | `protocol.h`     |
| `RIVR_PKT_DEFAULT_TTL`  | 7      | `protocol.h`     |
| `RIVR_OTA_SIG_LEN`      | 64     | `rivr_ota.h`     |
| `RIVR_OTA_KEY_LEN`      | 1      | `rivr_ota.h`     |
| `RIVR_OTA_SEQ_LEN`      | 4      | `rivr_ota.h`     |
| `RIVR_OTA_HDR_LEN`      | 69     | `rivr_ota.h`     |
| `RIVR_OTA_KEY_COUNT`    | 2      | `rivr_pubkey.h`  |
| `DEDUPE_CACHE_SIZE`     | 32     | `routing.h`      |
| `DEDUPE_EXPIRY_MS`      | 60000  | `routing.h`      |
| `BEACON_PAYLOAD_LEN`    | 11     | `protocol.h`     |
| `BEACON_CALLSIGN_MAX`   | 10     | `protocol.h`     |

---

## 8. Tooling

### Host-side Frame Decoder

`tools/rivr_decode` decodes RIVR wire frames from hex or binary input and
outputs JSONL:

```sh
# Decode from hex (one frame per line, space-separated hex bytes):
echo "52 56 01 01 00 07 00 00 00 ..." | rivr_decode

# Decode from binary file:
rivr_decode frame.bin

# Output (one JSON object per frame):
{"ok":true,"magic":"5256","version":1,"pkt_type":1,"type_name":"CHAT",
 "flags":0,"ttl":7,"hop":0,"net_id":0,"src_id":12345678,"dst_id":0,
 "seq":1,"pkt_id":1,"loop_guard":0,"payload_len":5,"payload_hex":"48656c6c6f","crc_ok":true}
```

See `tools/rivr_decode.c` for build instructions.
