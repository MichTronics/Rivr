# RIVR Language Reference

RIVR is a small stream-processing DSL.  A RIVR program consists of
**source declarations**, **`let` stream bindings**, and an **`emit` block**.

---

## Grammar (informal)

```
program   = stmt*
stmt      = source-decl | let-stmt | emit-stmt
source-decl = "source" IDENT ["@" IDENT] "=" source-kind ";"
let-stmt    = "let" IDENT "=" expr ";"
emit-stmt   = "emit" "{" (sink ";")+ "}"
expr        = primary ("|>" pipe-op)*
primary     = IDENT | STRING | INTEGER | "merge" "(" IDENT "," IDENT ")"
sink        = "io.usb.print"    "(" IDENT ")"
            | "io.lora.tx"      "(" IDENT ")"
            | "io.lora.beacon"  "(" IDENT ")"
            | "io.debug.dump"   "(" IDENT ")"
source-kind = "rf" | "usb" | "lora" | "programmatic"
            | "timer" "(" INTEGER ")"
```

---

## Source declarations

```rivr
source NAME [@CLOCK] = KIND;
```

| Parameter | Description |
|---|---|
| `NAME` | Identifier used to reference this source in `let` expressions |
| `@CLOCK` | Optional clock annotation (`@mono`, `@lmp`) |
| `KIND` | Hardware origin: `usb`, `lora`, `rf`, `programmatic`, or `timer(N)` |

**Source kinds:**

| Kind | Description |
|---|---|
| `rf` | LoRa radio receive stream — events are raw `Bytes` frames |
| `usb` | USB/UART receive stream |
| `lora` | Alias for `rf` |
| `programmatic` | Manually injected events (clock 0) |
| `timer(N)` | Periodic tick every N milliseconds (clock 0). Fires `Value::Int(mono_ms)` events automatically from the C timer table. |

**Clock ids:**

| Name | Id | Description |
|---|---|---|
| `mono` | 0 | Monotonic millisecond wall-clock |
| `lmp`  | 1 | Lamport logical clock (LoRa mesh) |

**Examples:**

```rivr
source rf_rx @lmp  = rf;          // LoRa receive → Lamport clock
source usb   @mono = usb;         // USB stream   → mono clock
source sensor      = programmatic; // clock 0 by default
source beacon_tick = timer(30000); // fires every 30 s, clock 0
```

---

## Pipe operators

### Text / transformation

| Operator | Description |
|---|---|
| `map.upper()` | Convert `Str` payload to ASCII uppercase |
| `filter.nonempty()` | Drop events with empty or whitespace-only `Str` payload |

### Filtering

| Operator | Description |
|---|---|
| `filter.kind("TAG")` | Pass only events whose string payload starts with `"TAG:"`. Use `"*"` to pass everything. |
| `filter.pkt_type(N)` | Pass only `Bytes` events where byte at offset 3 equals `N` (RIVR binary packet header). |

Packet type constants for `filter.pkt_type`:

| Constant | Value | Meaning |
|---|---|---|
| `PKT_CHAT` | 1 | Text message |
| `PKT_BEACON` | 2 | Node-presence advertisement |
| `PKT_ROUTE_REQ` | 3 | Route request |
| `PKT_ROUTE_RPL` | 4 | Route reply |
| `PKT_ACK` | 5 | Acknowledgement |
| `PKT_DATA` | 6 | Generic sensor data |
| `PKT_PROG_PUSH` | 7 | OTA program update (never relayed) |

### Aggregation

| Operator | Description |
|---|---|
| `fold.count()` | Replace each event with a running integer count |

### Windowing (ms-domain, clock 0)

| Operator | Description |
|---|---|
| `window.ms(N)` | Tumbling window of N milliseconds; emits a `Window` (list of strings) at each boundary |
| `throttle.ms(N)` | Forward at most one event per N ms |
| `debounce.ms(N)` | Forward the pending event only after N ms of silence |

### Windowing (tick-domain)

| Operator | Description |
|---|---|
| `window.ticks(N)` | Tumbling window of N ticks |
| `window.ticks(N, CAP, "POLICY")` | Capped window with overflow policy |
| `throttle.ticks(N)` | Forward at most one event per N ticks |
| `delay.ticks(N)` | Delay forwarding by N ticks |

**Window overflow policies:**

| Policy | Behaviour when buffer reaches CAP before tick boundary |
|---|---|
| `"drop_oldest"` | Evict oldest event, accept new one |
| `"drop_newest"` | Discard new event, keep existing buffer |
| `"flush_early"` | Emit full buffer immediately as `Window`, then accept new event |

### Rate limiting

| Operator | Description |
|---|---|
| `budget(RATE, BURST)` | Generic token-bucket: RATE events/s, BURST capacity |
| `budget.airtime(WINDOW_TICKS, DUTY)` | Radio duty-cycle by event count |
| `budget.toa_us(WINDOW_MS, DUTY, TOA_US)` | Radio duty-cycle by actual Time-on-Air |

**`budget.toa_us` parameters:**

| Parameter | Description |
|---|---|
| `WINDOW_MS` | Measurement window in ticks (= ms on clock 0) |
| `DUTY` | Maximum fraction of window used for TX (0.0–1.0) |
| `TOA_US` | Airtime cost per event in microseconds |

**Example** — SF9 BW125 kHz, 50-byte payload, 10% duty over 6 minutes:
```rivr
|> budget.toa_us(360000, 0.10, 360000)
```
Computed budget: `360 000 × 0.10 × 1 000 = 36 000 000 µs` per window.
Max packets: `36 000 000 / 360 000 = 100`.

### Fan-in

```rivr
let combined = merge(source_a, source_b);
```

Merges two streams into one (events from both pass through).

### Debug

| Operator | Description |
|---|---|
| `tag("LABEL")` | Attach a trace label string to every passing event |

---

## Emit / sinks

```rivr
emit {
  io.lora.tx(STREAM);
  io.usb.print(STREAM);
  io.debug.dump(STREAM);
}
```

| Sink | C callback | Behaviour |
|---|---|---|
| `io.lora.tx` | `rf_tx_sink_cb` | Encode and push to `rf_tx_queue` |
| `io.lora.beacon` | `beacon_sink_cb` | Build `PKT_BEACON` (callsign + hop_count) and push to `rf_tx_queue` |
| `io.usb.print` | `usb_print_sink_cb` | `printf` to UART stdout |
| `io.debug.dump` | `log_sink_cb` | ESP-IDF `ESP_LOGI` |

Multiple streams can target the same sink:

```rivr
emit {
  rf_tx(chat);
  usb_print(chat);   // same stream, two sinks
}
```

---

## Complete examples

### Default mesh pipeline (with beacon)

```rivr
source rf_rx @lmp = rf;
source beacon_tick = timer(30000);

let chat = rf_rx
  |> filter.pkt_type(1)
  |> budget.toa_us(280000, 0.10, 280000)
  |> throttle.ticks(1);

emit { io.lora.tx(chat); }
emit { io.lora.beacon(beacon_tick); }
```

### Multi-type mesh routing (extended mesh program)

```rivr
source rf_rx @lmp = rf;
source beacon_tick = timer(30000);

let chat = rf_rx
  |> filter.pkt_type(1)
  |> budget.toa_us(280000, 0.10, 280000)
  |> throttle.ticks(1);

let data = rf_rx
  |> filter.pkt_type(6)
  |> throttle.ticks(1);

emit { io.lora.tx(chat); }
emit { io.lora.tx(data); }
emit { io.lora.beacon(beacon_tick); }
```

### Capped window with flush-early policy

```rivr
source rf @lmp = rf;

let burst = rf
  |> window.ticks(10, 3, "flush_early");

emit { io.usb.print(burst); }
```

### Multi-source merge

```rivr
source usb  = usb;
source lora = lora;

let both = merge(usb, lora)
  |> map.upper()
  |> filter.nonempty();

emit { io.usb.print(both); }
```

---

## Parser rules and edge cases

- Comments: `// single-line only`
- Identifiers: ASCII alphanumeric + underscore
- String literals: double-quoted, supports `\\`, `\"`, `\n`, `\t`
- Integers: decimal only (no `0x` hex in RIVR source)
- Floats: plain decimal (e.g. `0.10`, not `10%`)
- `filter.pkt_type(N)`: N must be 0–255
- Operators are dot-namespaced: `budget.toa_us`, `filter.pkt_type`, etc.
- The `|>` token must have no space between `|` and `>`

---

## OpCode table (internal)

| OpCode | Hex | Operator |
|---|---|---|
| `Source` | 0x00 | source pass-through |
| `MapUpper` | 0x10 | `map.upper()` |
| `FilterNonempty` | 0x11 | `filter.nonempty()` |
| `FilterKind` | 0x12 | `filter.kind("K")` |
| `FilterPktType` | 0x13 | `filter.pkt_type(N)` |
| `FoldCount` | 0x20 | `fold.count()` |
| `WindowTicks` | 0x30 | `window.ticks(N)` |
| `ThrottleTicks` | 0x31 | `throttle.ticks(N)` |
| `DelayTicks` | 0x32 | `delay.ticks(N)` |
| `WindowMs` | 0x38 | `window.ms(N)` |
| `ThrottleMs` | 0x39 | `throttle.ms(N)` |
| `DebounceMs` | 0x3A | `debounce.ms(N)` |
| `Budget` | 0x40 | `budget(r,b)` |
| `AirtimeBudget` | 0x41 | `budget.airtime(…)` |
| `ToaBudget` | 0x42 | `budget.toa_us(…)` |
| `Merge` | 0x50 | `merge(a,b)` |
| `Tag` | 0x51 | `tag("label")` |
| `Emit` | 0x60 | `emit { … }` |
