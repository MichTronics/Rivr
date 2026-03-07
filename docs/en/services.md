# RIVR Application Services

> **Status:** Stable (protocol v1 extensions)  
> **Source of truth:** `firmware_core/protocol.h`, `rivr_layer/rivr_svc.h`, `rivr_layer/rivr_svc.c`  
> **Depends on:** routing layer (ACK/retry, §3), RIVR engine (§4)

---

## 1. Overview

The application services layer sits immediately above the routing stack.  It
provides four standardised packet types that cover the most common embedded
mesh use-cases without dynamic allocation and within the 231-byte maximum
payload constraint of the RIVR wire format.

| Service | PKT type | ID | Direction | Fixed payload | Max text |
|---------|----------|----|-----------|--------------|----------|
| CHAT | `PKT_CHAT` | 1 | any–any | — (free-form) | 224 B |
| TELEMETRY | `PKT_TELEMETRY` | 8 | sensor → gateway | 11 B | — |
| MAILBOX | `PKT_MAILBOX` | 9 | any → any | 7-byte header + text | `RIVR_PKT_MAX_PAYLOAD − 7` |
| ALERT | `PKT_ALERT` | 10 | any → all | 7 B | — |

All constants are defined in `firmware_core/protocol.h`.

---

## 2. Service Dispatch

The C-layer dispatch lives in `rivr_layer/rivr_sources.c`, step **5d**,
immediately after route-reply processing and before the frame is injected into
the RIVR engine.  This ordering guarantees:

1. Routing tables are up to date before service handlers run.
2. Service handlers can compare `recipient_id` against the freshly-updated
   `g_my_node_id`.
3. The RIVR engine receives every frame regardless of whether a service
   handler consumed it (non-exclusive dispatch).

```c
/* ── rivr_sources.c, step 5d ── */
if (pkt_hdr.pkt_type == PKT_CHAT)
    handle_chat_message(&pkt_hdr, payload, len, frame.rssi_dbm);
if (pkt_hdr.pkt_type == PKT_TELEMETRY)
    handle_telemetry_publish(&pkt_hdr, payload, len);
if (pkt_hdr.pkt_type == PKT_MAILBOX)
    handle_mailbox_store(&pkt_hdr, payload, len, now_ms);
if (pkt_hdr.pkt_type == PKT_ALERT)
    handle_alert_event(&pkt_hdr, payload, len);
```

Each handler logs a structured `@`-prefixed JSON record to UART (see §7) and
then returns; relay/ACK decisions are made by the surrounding C-layer, not the
handler.

---

## 3. CHAT (`PKT_CHAT = 1`)

PKT_CHAT predates the application services layer and retains its original free-
form payload.  `handle_chat_message()` provides a uniform log record and calls
the role-specific UI hook.

### Payload
Free-form UTF-8 text, 0–`RIVR_PKT_MAX_PAYLOAD` bytes.  No length prefix.

### Handler behaviour
- Emits a `@CHT` log record (§7.1) on every reception.
- Calls `rivr_cli_on_chat_rx()`, which is a zero-cost inline stub on
  non-client builds (`#ifndef RIVR_ROLE_CLIENT`).

### Routing semantics
- `dst_id = 0` → broadcast (all nodes receive and relay).
- `dst_id = <id>` → unicast via route cache; ACK/retry applies.

---

## 4. TELEMETRY (`PKT_TELEMETRY = 8`)

Compact fixed-size frame for periodic numeric sensor readings from low-power
nodes.  Designed to be generated without `printf` or `sprintf`.

### Payload (11 bytes, little-endian)

```
┌─────────┬──────┬────────────┬───────────────────────────────────────────────┐
│  Offset │ Size │ Type       │ Description                                   │
├─────────┼──────┼────────────┼───────────────────────────────────────────────┤
│    0    │  2   │ u16 LE     │ sensor_id — application-defined sensor index  │
│    2    │  4   │ i32 LE     │ value — scaled integer (unit-dependent)       │
│    6    │  1   │ u8         │ unit_code — UNIT_* constant (see §4.1)        │
│    7    │  4   │ u32 LE     │ timestamp — seconds since node boot (0=unset) │
└─────────┴──────┴────────────┴───────────────────────────────────────────────┘
```

Total: `SVC_TELEMETRY_PAYLOAD_LEN = 11`

### 4.1 Unit Codes

| Constant | Value | Meaning | Scale factor |
|----------|-------|---------|--------------|
| `UNIT_NONE` | 0 | dimensionless / raw | 1 |
| `UNIT_CELSIUS` | 1 | temperature | ×100 (2500 = 25.00 °C) |
| `UNIT_PERCENT_RH` | 2 | relative humidity | ×100 (5500 = 55.00 %) |
| `UNIT_MILLIVOLTS` | 3 | voltage | ×1 (3300 = 3300 mV) |
| `UNIT_DBM` | 4 | RSSI / TX power | ×1 signed (−80 = −80 dBm) |
| `UNIT_PPM` | 5 | concentration | ×100 (40000 = 400.00 ppm) |
| `UNIT_CUSTOM` | 255 | application-defined | 1 |

### Handler behaviour
- Validates `payload_len >= SVC_TELEMETRY_PAYLOAD_LEN`.
- Decodes all fields with `memcpy` (no alignment assumption).
- Emits a `@TEL` log record (§7.2).

### Routing semantics
- Typically broadcast (`dst_id = 0`) from sensor nodes.
- Relay nodes gate re-transmission with `budget.toa_us` in RIVR programs.

---

## 5. MAILBOX (`PKT_MAILBOX = 9`)

Store-and-forward primitive for nodes that are intermittently reachable.  The
C-layer maintains a static 8-entry ring-buffer for messages addressed to this
node; the RIVR program controls when buffered frames are re-forwarded.

### Payload (variable, max `RIVR_PKT_MAX_PAYLOAD`)

```
┌─────────┬──────┬────────────┬───────────────────────────────────────────────┐
│  Offset │ Size │ Type       │ Description                                   │
├─────────┼──────┼────────────┼───────────────────────────────────────────────┤
│    0    │  4   │ u32 LE     │ recipient_id — final destination (0 = any)    │
│    4    │  2   │ u16 LE     │ msg_seq — per-source sequence counter         │
│    6    │  1   │ u8         │ flags — MB_FLAG_* bitmask (see §5.1)          │
│    7    │  N   │ UTF-8      │ text body (0..SVC_MAILBOX_MAX_TEXT bytes)     │
└─────────┴──────┴────────────┴───────────────────────────────────────────────┘
```

Header: `SVC_MAILBOX_HDR_LEN = 7`  
Max text: `SVC_MAILBOX_MAX_TEXT = RIVR_PKT_MAX_PAYLOAD − SVC_MAILBOX_HDR_LEN`

### 5.1 Mailbox Flags

| Constant | Value | Meaning |
|----------|-------|---------|
| `MB_FLAG_NEW` | 0x01 | Message not yet delivered to recipient |
| `MB_FLAG_DELIVERED` | 0x02 | Recipient ACK received by originator |
| `MB_FLAG_FORWARD` | 0x04 | Frame is a relay copy, not the original |

### 5.2 In-RAM Mailbox Store

```c
/* rivr_layer/rivr_svc.h */
#define MB_STORE_CAP  8u

typedef struct {
    uint32_t src_id, recipient_id;
    uint16_t msg_seq;
    uint8_t  flags, text_len;
    char     text[SVC_MAILBOX_MAX_TEXT + 1u]; /* NUL-terminated */
    uint32_t stored_at_ms;
    bool     valid;
} mailbox_entry_t;

typedef struct {
    mailbox_entry_t entries[MB_STORE_CAP];
    uint8_t         head, count;
} mailbox_store_t;

extern mailbox_store_t g_mailbox_store; /* BSS, zero-initialised */
```

- **Capacity:** 8 entries (`MB_STORE_CAP`).
- **Eviction policy:** LRU ring — head pointer advances and overwrites the
  oldest entry when the store is full.
- **Allocation:** entirely within the BSS segment; no heap (`malloc`) used.
- **Persistence:** not persisted across resets; a `delivered` flag sweep
  can be added by the application on reconnect.

### Handler behaviour
- Validates `payload_len >= SVC_MAILBOX_HDR_LEN`.
- If `recipient_id == g_my_node_id` **or** `recipient_id == 0`:
  stores into `g_mailbox_store` and sets `stored = true` in the log record.
- Always emits a `@MAIL` log record (§7.3) regardless of whether stored.

### Routing semantics
- Unicast (`dst_id` = next-hop towards recipient) when the sender has a
  route; broadcast (`dst_id = 0`) when route is unknown.
- Relay nodes buffer and re-forward under RIVR program gating (see
  `examples/store_forward_mailbox.rivr`).

---

## 6. ALERT (`PKT_ALERT = 10`)

Priority event notification — fire once, propagate everywhere.  The alert
payload is compact enough to fit in a single slot even on lossy links.

### Payload (7 bytes, little-endian)

```
┌─────────┬──────┬────────────┬───────────────────────────────────────────────┐
│  Offset │ Size │ Type       │ Description                                   │
├─────────┼──────┼────────────┼───────────────────────────────────────────────┤
│    0    │  1   │ u8         │ severity — ALERT_SEV_* constant (see §6.1)    │
│    1    │  2   │ u16 LE     │ alert_code — application-defined event code   │
│    3    │  4   │ i32 LE     │ value — associated numeric data (e.g. temp)   │
└─────────┴──────┴────────────┴───────────────────────────────────────────────┘
```

Total: `SVC_ALERT_PAYLOAD_LEN = 7`

### 6.1 Alert Severity Levels

| Constant | Value | Colour | Behaviour |
|----------|-------|--------|-----------|
| `ALERT_SEV_INFO` | 1 | — | Log only |
| `ALERT_SEV_WARN` | 2 | yellow | Log + human-readable `[!] ALERT` line |
| `ALERT_SEV_CRIT` | 3 | red | Log + `[!] ALERT` + `RIVR_LOGW()` |

### Handler behaviour
- Validates `payload_len >= SVC_ALERT_PAYLOAD_LEN`.
- Emits a `@ALERT` log record (§7.4) on every reception.
- For `ALERT_SEV_WARN` and `ALERT_SEV_CRIT`: also prints a human-readable
  `[!] ALERT` line with decoded fields.
- For `ALERT_SEV_CRIT`: additionally calls `RIVR_LOGW()` to surface the
  event in the ESP-IDF log stream.

### Routing semantics
- Always broadcast (`dst_id = 0`); routed alerts use `PKT_DATA` instead.
- Relay nodes assign elevated duty-cycle budget:
  `budget.toa_us(300000, 0.20, 280000)` (20 %, see `chat_relay.rivr`).

---

## 7. Structured Log Output

Each handler prints a single-line JSON record to UART (stdout / USB serial)
prefixed with `@` so host tools can filter it out of arbitrary debug output.
All records end with `\r\n`.

Log output is produced with `printf()` — no buffering, no heap.

### 7.1 `@CHT` — Chat message received

```
@CHT {"src":"0x<HEX8>","dst":"0x<HEX8>","rssi":<dBm>,"len":<N>,"text":"<TEXT>"}
```

| Field | Type | Description |
|-------|------|-------------|
| `src` | hex string | Sender node ID |
| `dst` | hex string | Destination node ID (0x00000000 = broadcast) |
| `rssi` | signed integer | Received signal strength in dBm |
| `len` | unsigned integer | Payload length in bytes |
| `text` | JSON string | Message body (`"` → `\"`, `\` → `\\`, control/non-ASCII → `.`) |

### 7.2 `@TEL` — Telemetry reading published

```
@TEL {"src":"0x<HEX8>","sid":<N>,"val":<V>,"unit":<U>,"unit_str":"<name>","ts":<T>}
```

| Field | Type | Description |
|-------|------|-------------|
| `src` | hex string | Sensor node ID |
| `sid` | unsigned integer | Sensor ID (application-defined) |
| `val` | signed integer | Scaled sensor value |
| `unit` | unsigned integer | `UNIT_*` numeric code |
| `unit_str` | string | Human-readable unit abbreviation (e.g. `"C*100"`, `"mV"`, `"ppm*100"`) |
| `ts` | unsigned integer | Node-boot-relative timestamp in seconds (0 = unset) |

### 7.3 `@MAIL` — Mailbox frame received

```
@MAIL {"src":"0x<HEX8>","to":"0x<HEX8>","seq":<N>,"flags":<F>,"stored":<bool>,"text":"<TEXT>"}
```

| Field | Type | Description |
|-------|------|-------------|
| `src` | hex string | Originating node ID |
| `to` | hex string | Intended recipient ID (0x00000000 = any store node) |
| `seq` | unsigned integer | Per-source message sequence number |
| `flags` | unsigned integer | `MB_FLAG_*` bitmask |
| `stored` | `true` / `false` | Whether the frame was saved in `g_mailbox_store` |
| `text` | JSON string | Message body (same sanitisation as `@CHT`) |

### 7.4 `@ALERT` — Alert event received

```
@ALERT {"src":"0x<HEX8>","sev":<S>,"sev_str":"<name>","code":<C>,"val":<V>}
```

| Field | Type | Description |
|-------|------|-------------|
| `src` | hex string | Originating node ID |
| `sev` | unsigned integer | `ALERT_SEV_*` numeric code |
| `sev_str` | string | `"INFO"`, `"WARN"`, `"CRIT"`, or `"?"` for unknown |
| `code` | unsigned integer | Application-defined alert code |
| `val` | signed integer | Associated numeric value |

---

## 8. RIVR Program Integration

The RIVR engine sees every received frame (step 6 in `rivr_sources.c`),
including service frames.  RIVR programs gate relay — not reception.

### Typical patterns

**Relay all service traffic with duty-cycle gating:**

```rivr
source rf_rx @lmp = rf;
let tel   = rf_rx |> filter.pkt_type(8)  |> budget.toa_us(300000, 0.10, 280000) |> throttle.ticks(1);
let alert = rf_rx |> filter.pkt_type(10) |> budget.toa_us(300000, 0.20, 280000);
emit { io.lora.tx(tel);   }
emit { io.lora.tx(alert); }
```

**Store-and-forward mailbox with window buffering:**

```rivr
source rf_rx @lmp = rf;
let mail = rf_rx
  |> filter.pkt_type(9)
  |> window.ticks(12, 8, "drop_oldest")
  |> delay.ticks(3)
  |> budget.toa_us(600000, 0.04, 280000);
emit { io.lora.tx(mail); }
emit { io.usb.print(mail); }
```

**Aggregate telemetry reception in a 30-second window:**

```rivr
source rf_rx @lmp = rf;
let count = rf_rx |> filter.pkt_type(8) |> fold.count();
let win   = rf_rx |> filter.pkt_type(8) |> window.ms(30000);
emit { io.usb.print(count); }  // running total
emit { io.usb.print(win);   }  // window events for outage detection
```

See `examples/` for complete programs:

- [`examples/chat_relay.rivr`](../../examples/chat_relay.rivr) — all-services relay
- [`examples/store_forward_mailbox.rivr`](../../examples/store_forward_mailbox.rivr) — buffered mailbox
- [`examples/telemetry_periodic.rivr`](../../examples/telemetry_periodic.rivr) — telemetry aggregation + heartbeat

---

## 9. Adding a New Service

1. Reserve a `PKT_*` constant in `firmware_core/protocol.h` (next is 11).
2. Define payload length, field layout, and any unit/flag constants there.
3. Declare the handler in `rivr_layer/rivr_svc.h`.
4. Implement the handler in `rivr_layer/rivr_svc.c` following the same
   BSS-only / no-`malloc` pattern.
5. Add dispatch in `rivr_sources.c` step 5d.
6. Update `firmware_core/CMakeLists.txt` if a new `.c` file is created.
7. Add `filter.pkt_type(N)` usage in the relevant example program.
8. Document the payload format and log record in this file.
