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
rivr_result_t rivr_engine_init(const char *program);
```

`program` is a NUL-terminated RIVR source string.
`rivr_embed_init()` automatically tries NVS first; the compiled-in
`RIVR_ACTIVE_PROGRAM` is used as a fallback on first boot.

| `rc.code` | Meaning |
|---|---|
| `RIVR_OK` | Success — engine ready |
| `RIVR_ERR_PARSE` | Parse error in source string |
| `RIVR_ERR_COMPILE` | Compile/semantic error |

**Example:**

```c
rivr_result_t rc = rivr_engine_init(RIVR_DEFAULT_PROGRAM);
if (rc.code != RIVR_OK) {
    ESP_LOGE(TAG, "RIVR init failed: %" PRIu32, rc.code);
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
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

### 4 — Run the scheduler

```c
rivr_result_t rivr_engine_run(uint32_t max_steps);
```

Called inside `rivr_tick()` every main-loop iteration.
`max_steps` caps the number of scheduler cycles to prevent starvation.
`rc.cycles_used` reports actual work done; `rc.gas_remaining == 0` signals
that the engine was still busy at the step limit (potential tick-storm).

**Main loop pattern (from `rivr_embed.c`):**

```c
// Call radio_service_rx() BEFORE rivr_tick() every iteration.
// The ISR (radio_isr) only sets s_dio1_pending; all SPI work happens here.
void main_loop_body(void) {
    radio_service_rx();      // drain DIO1 events via SPI (main task only)
    rivr_tick();             // drain ring-buffer → engine → emit
    tx_drain_loop();         // send queued TX frames with duty-cycle gate
}

void rivr_tick(void) {
    sources_rf_rx_drain();   // inject radio frames
    sources_cli_drain();     // inject CLI events
    sources_timer_drain();   // fire due timer sources
    rivr_engine_run(256);    // run DAG
}
```

### 5 — NVS program storage

```c
bool rivr_nvs_store_program(const char *src);
```

Writes `src` to NVS key `rivr/program` (max 2047 bytes).  On the next boot
(or after `rivr_embed_reload()`) this program will be used instead of the
compiled-in default.

```c
bool rivr_embed_reload(void);
```

Hot-reloads the engine at runtime: resets the timer table, re-initialises
the engine from NVS (or fallback), and re-enumerates timer sources.
Called automatically from the main loop when `g_program_reload_pending` is set
(e.g. after a `PKT_PROG_PUSH` frame arrives).

### 6 — Timer source registration

`rivr_embed_init()` and `rivr_embed_reload()` automatically enumerate all
`source NAME = timer(N)` declarations via:

```c
typedef void (*rivr_timer_source_cb_t)(const char *name, uint64_t interval_ms);
void rivr_foreach_timer_source(rivr_timer_source_cb_t cb);
```

The C timer table (`rivr_timer_entry_t s_timers[RIVR_TIMER_MAX]`) is populated
by `sources_register_timer()` and polled by `sources_timer_drain()` each tick.

### 7 — Query clocks

```c
uint64_t rivr_engine_clock_now(uint8_t clock_id);
```

Returns the current timestamp for the given clock id (0 = mono, 1 = lmp)
from the engine's internal state after the last `rivr_engine_run()` call.

---

## Sink registration

Sinks are registered by name in `rivr_sinks_init()` via `rivr_register_sink()`:

```c
rivr_register_sink("io.lora.tx",     rf_tx_sink_cb,     NULL);
rivr_register_sink("io.lora.beacon", beacon_sink_cb,    NULL);
rivr_register_sink("io.usb.print",   usb_print_sink_cb, NULL);
rivr_register_sink("io.debug.dump",  log_sink_cb,       NULL);
```

The `beacon_sink_cb` builds a `PKT_BEACON` frame containing the node's
callsign (`g_callsign`), net id (`g_net_id`), and hop count, then pushes
it to `rf_tx_queue`.

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
| `PKT_PROG_PUSH` | 7 |

`PKT_PROG_PUSH` carries a NUL-terminated RIVR source string as payload.
On receipt the firmware stores it to NVS and sets `g_program_reload_pending`.
It is **never relayed** over the mesh.

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

Typical usage in the main loop (**not** in the ISR — SPI calls are illegal from interrupt context on ESP32):

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

## Rivr Fabric API

`firmware_core/rivr_fabric.h` provides congestion-aware relay suppression.
It is active only when `RIVR_FABRIC_REPEATER=1`; all functions compile to
no-ops otherwise.

### Lifecycle

```c
void rivr_fabric_init(void);          // call once in app_main after platform_init
void rivr_fabric_tick(uint32_t now_ms); // call every main-loop iteration
```

### Traffic events (feed from radio / tx path)

```c
void rivr_fabric_on_rx(void);            // called after each received frame
void rivr_fabric_on_tx_enqueued(void);   // relay frame entered the TX queue
void rivr_fabric_on_tx_ok(void);         // TX completed successfully
void rivr_fabric_on_tx_fail(void);       // TX failed (timeout / PA error)
void rivr_fabric_on_tx_blocked_dc(void); // TX dropped by duty-cycle guard
```

### Relay decision

```c
fabric_decision_t rivr_fabric_decide_relay(uint8_t pkt_type, uint32_t now_ms);
```

Returns one of:

| Value | Meaning |
|---|---|
| `FABRIC_SEND_NOW` | Relay immediately |
| `FABRIC_DELAY` | Add jitter delay (score ≥ `RIVR_FABRIC_DELAY_THRESHOLD`) |
| `FABRIC_DROP` | Suppress relay entirely (score ≥ `RIVR_FABRIC_DROP_THRESHOLD`) |

Only `PKT_CHAT` and `PKT_DATA` are gated; all other types always return
`FABRIC_SEND_NOW`.  When the score reaches `RIVR_FABRIC_BLACKOUT_GUARD_SCORE`
the return value is clamped to `FABRIC_DELAY` (max delay) to prevent a
complete relay blackout.

### Debug / display

```c
typedef struct {
    uint8_t  score;           // 0–100 congestion score
    uint8_t  band;            // 0=OK 1=LIGHT_DLY 2=DLY 3=DROP
    uint32_t rx_per_s;        // RX frames averaged over 60 s window
    uint32_t blocked_per_s;   // duty-cycle blocks averaged over window
    uint32_t fail_per_s;      // TX failures averaged over window
    uint32_t relay_drop_total;
    uint32_t relay_delay_total;
} fabric_debug_t;

void rivr_fabric_get_debug(uint32_t now_ms, fabric_debug_t *out);
```

### Scalar getters

```c
uint8_t  rivr_fabric_get_score(void);
uint32_t rivr_fabric_get_relay_delayed(void);
uint32_t rivr_fabric_get_relay_dropped(void);
uint32_t rivr_fabric_get_relay_total(void);
uint32_t rivr_fabric_get_tx_blocked(void);
uint32_t rivr_fabric_get_rx_total(void);
```

### Threshold macros (all `#ifndef`-guarded)

| Macro | Default | Description |
|---|---|---|
| `RIVR_FABRIC_DROP_THRESHOLD` | `80` | Score ≥ this → DROP |
| `RIVR_FABRIC_DELAY_THRESHOLD` | `50` | Score ≥ this → DELAY |
| `RIVR_FABRIC_LIGHT_DELAY_THRESHOLD` | `20` | Score ≥ this → short jitter |
| `RIVR_FABRIC_MAX_EXTRA_DELAY_MS` | `1000` | Maximum DELAY in milliseconds |
| `RIVR_FABRIC_BLACKOUT_GUARD_SCORE` | `95` | Clamp DROP → DELAY at this score |

### Periodic log

When `RIVR_FABRIC_REPEATER=1`, `main.c` logs a `[FAB]` line every 5 s:

```
I (...) MAIN: [FAB] score=34 rx=12 relay=8 delay=2 drop=0 dc_block=1
```

---

## Node variants

RIVR supports compile-time role selection through variant headers.  Each
variant header lives in `variants/<board>/config.h` and is force-included
by PlatformIO via `-include`.  Every macro is wrapped in `#ifndef` so any
`-D` flag overrides it.

### Available roles

| Macro | Role |
|---|---|
| *(none set)* | Standard node — full relay of all packet types |
| `RIVR_BUILD_REPEATER=1` + `RIVR_FABRIC_REPEATER=1` | Repeater — full relay with Fabric congestion gating for `PKT_CHAT`/`PKT_DATA` |
| `RIVR_ROLE_CLIENT=1` | Client — receives `PKT_CHAT`/`PKT_DATA` locally, suppresses their relay; control frames still forwarded |

### Client relay suppression

In `rivr_layer/rivr_sources.c`, the following guard skips enqueuing relay
frames for chat and data packets when compiled as a client:

```c
#ifdef RIVR_ROLE_CLIENT
    if (fwd_type == PKT_CHAT || fwd_type == PKT_DATA) {
        goto skip_enqueue;   // suppress relay; deliver locally only
    }
#endif
```

Control frames (`PKT_BEACON`, `PKT_ROUTE_REQ`, `PKT_ROUTE_RPL`, `PKT_ACK`,
`PKT_PROG_PUSH`) are not affected and relay normally so mesh routing stays
intact.

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
5. Call the lifecycle sequence: `rivr_embed_init()` → main loop calling `rivr_tick()` + `tx_drain_loop()`.
6. Optionally call `rivr_nvs_store_program()` to persist user programs.
