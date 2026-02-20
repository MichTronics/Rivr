# RIVR — Firmware Integration

This guide covers integrating `rivr_core` (the Rust static library) into a C
firmware project — either with the provided ESP32 layer or on a new platform.

---

## C header: `rivr_embed.h`

Include this file in any C translation unit that calls RIVR:

```c
#include "rivr_embed.h"
```

---

## Value types

```c
/* Tag byte stored in rivr_value_t.tag */
#define RIVR_VAL_UNIT   0
#define RIVR_VAL_INT    1
#define RIVR_VAL_BOOL   2
#define RIVR_VAL_STR    3
#define RIVR_VAL_BYTES  4
#define RIVR_VAL_WINDOW 5

typedef struct {
    uint8_t  tag;        /* RIVR_VAL_* constant */
    uint32_t len;        /* byte length of data  */
    uint8_t  data[256];  /* payload              */
} rivr_value_t;
```

`RIVR_VAL_BYTES` is used for raw LoRa frames.
`filter.pkt_type(N)` checks `value.data[3] == N` (offset = `PKT_TYPE_BYTE_OFFSET`).

---

## Event type

```c
typedef struct {
    uint8_t       source_id;  /* which source (0-based index) */
    uint8_t       clock_id;   /* 0 = mono, 1 = lmp             */
    uint64_t      timestamp;  /* ms (mono) or ticks (lmp)      */
    rivr_value_t  value;
} rivr_event_t;
```

---

## Lifecycle API

### 1 — Initialise the engine

```c
int rivr_engine_init(const char *program, size_t len);
```

| Return | Meaning |
|---|---|
| `0` | Success — engine ready |
| `-1` | Parse error |
| `-2` | Compile error |

`program` is a NUL-terminated RIVR source string.
In the ESP32 build the active program is selected via `RIVR_ACTIVE_PROGRAM` in `default_program.h`.

**Example:**

```c
static const char PROG[] =
    "source rf_rx @lmp = rf_rx;\n"
    "let chat = rf_rx |> filter.pkt_type(1) |> throttle.ticks(1);\n"
    "emit { rf_tx(chat); }\n";

if (rivr_engine_init(PROG, strlen(PROG)) != 0) {
    ESP_LOGE(TAG, "RIVR init failed");
    abort();
}
```

### 2 — Register the emit dispatcher

```c
typedef void (*rivr_emit_fn)(uint8_t sink_id,
                             const rivr_value_t *value);

void rivr_set_emit_dispatch(rivr_emit_fn fn);
```

Register your dispatch callback **before** the first `rivr_engine_run()` call.
`sink_id` is the zero-based index of the `emit { … }` clause that fired.

**Example dispatcher (ESP32):**

```c
static void emit_dispatch(uint8_t sink_id, const rivr_value_t *v) {
    if (sink_id == SINK_RF_TX && v->tag == RIVR_VAL_BYTES) {
        rf_enqueue(v->data, v->len);
    } else if (sink_id == SINK_USB_PRINT && v->tag == RIVR_VAL_STR) {
        printf("[RIVR] %.*s\n", (int)v->len, v->data);
    }
}

rivr_set_emit_dispatch(emit_dispatch);
```

### 3 — Inject events

```c
void rivr_inject_event(const rivr_event_t *event);
```

Call once per received frame or sensor reading.
Safe to call from any task; the event is immediately processed (no internal queue).

**LoRa RX example:**

```c
rivr_event_t ev = {
    .source_id = SOURCE_RF_RX,
    .clock_id  = 1,                  /* lmp */
    .timestamp = lmp_clock_now(),
    .value = {
        .tag = RIVR_VAL_BYTES,
        .len = frame_len,
    }
};
memcpy(ev.value.data, frame_buf, frame_len);
rivr_inject_event(&ev);
```

### 4 — Advance the clock / tick operators

```c
void rivr_engine_run(uint64_t clock0_ms, uint64_t clock1_ticks);
```

Call **every main-loop iteration** to tick time-based operators
(`window.ms`, `throttle.ms`, `debounce.ms`, etc.).

**Main loop pattern:**

```c
while (1) {
    uint64_t now_ms    = esp_timer_get_time() / 1000;
    uint64_t now_ticks = lmp_clock_now();
    rivr_engine_run(now_ms, now_ticks);
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

### 5 — Query clocks

```c
uint64_t rivr_engine_clock_now(uint8_t clock_id);
```

Returns the current timestamp for the given clock id (0 = mono, 1 = lmp)
from the engine's internal state after the last `rivr_engine_run()` call.

---

## Sink registration helper

`rivr_layer/rivr_sources.c` provides convenience macros:

```c
#define RIVR_REGISTER_SINK(name, fn_ptr)
```

```c
rivr_register_sink(SINK_RF_TX,    rf_tx_sink_cb);
rivr_register_sink(SINK_USB_PRINT, usb_print_sink_cb);
rivr_register_sink(SINK_LOG,      log_sink_cb);
```

---

## Binary protocol

Frames on the wire follow the RIVR binary packet format defined in
`firmware_core/protocol.h`.

### Header layout

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;       /* 0xR1 */
    uint8_t  version;     /* protocol version */
    uint8_t  hop_limit;   /* decremented per hop */
    uint8_t  pkt_type;    /* PKT_CHAT=1 … PKT_DATA=6 */
    uint32_t src_node_id; /* originating node */
    uint32_t seq_no;      /* per-source sequence number */
    uint8_t  payload[];   /* variable-length payload */
} rivr_pkt_hdr_t;
```

### Packet types

| Constant | Value |
|---|---|
| `PKT_CHAT` | 1 |
| `PKT_BEACON` | 2 |
| `PKT_ROUTE_REQ` | 3 |
| `PKT_ROUTE_RPL` | 4 |
| `PKT_ACK` | 5 |
| `PKT_DATA` | 6 |

### Encode / decode

```c
/* Encode an outbound struct → bytes (returns number of bytes written) */
int protocol_encode(const rivr_pkt_hdr_t *hdr,
                    const uint8_t *payload, size_t payload_len,
                    uint8_t *out_buf,       size_t out_cap);

/* Decode an inbound byte buffer → struct */
int protocol_decode(const uint8_t *buf,  size_t len,
                    rivr_pkt_hdr_t *hdr,
                    uint8_t *payload_out, size_t payload_cap);
```

Both functions return `0` on success, negative error code on failure.
`protocol_decode` validates the CRC-16 appended to every frame.

---

## Routing API

`firmware_core/routing.h` provides lightweight deduplication and forwarding
decisions without heap allocation.

```c
/* Returns non-zero if this (src, seq_no) pair was already seen recently */
int routing_dedupe_check(uint32_t src_node_id, uint32_t seq_no);

/* Returns non-zero if the frame should be forwarded based on hop_limit */
int routing_should_forward(const rivr_pkt_hdr_t *hdr);

/* Update neighbour table with signal-quality information */
void routing_update_neighbour(uint32_t node_id, int8_t rssi, int8_t snr);
```

Typical usage in the LoRa RX interrupt handler / main loop:

```c
rivr_pkt_hdr_t hdr;
uint8_t payload[128];

if (protocol_decode(raw, raw_len, &hdr, payload, sizeof(payload)) < 0)
    return;                                        // bad CRC → drop

if (routing_dedupe_check(hdr.src_node_id, hdr.seq_no))
    return;                                        // duplicate → drop

if (routing_should_forward(&hdr))
    rf_enqueue(raw, raw_len);                      // re-broadcast

rivr_inject_event(&(rivr_event_t){ ... });         // feed local pipeline
```

---

## Simulation mode

Compile-time flags that replace real hardware:

| Flag | Value | Effect |
|---|---|---|
| `RIVR_SIM_MODE` | `1` | Skip `platform_init()`; call `radio_init_buffers_only()` |
| `RIVR_SIM_TX_PRINT` | `1` | Print `[SIM TX]` to UART instead of transmitting over SX1262 |

Set via PlatformIO:

```ini
[env:esp32_sim]
build_flags = -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1
```

Or via idf.py:

```powershell
idf.py -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1 build
```

---

## Porting checklist (new platform)

1. Implement `platform_init()` and `platform_clock_ms()` for your MCU.
2. Provide a UART driver and wire it to `usb_print_sink_cb`.
3. Provide a radio driver with `radio_transmit(buf, len)` and an RX ring-buffer.
4. Link `librivr_core.a` (Rust, cross-compiled for your target).
5. Call the lifecycle sequence: `rivr_engine_init` → `rivr_set_emit_dispatch` → loop `rivr_inject_event` + `rivr_engine_run`.
