# RIVR — Project Overview

RIVR is a **stream-graph runtime and domain-specific language** designed for
resource-constrained LoRa mesh nodes, implemented in Rust and C.

## What problem does RIVR solve?

LoRa mesh devices must process radio frames under strict constraints:

| Constraint | Consequence |
|---|---|
| **Duty cycle** (≤ 1 % or ≤ 10 %) | Cannot retransmit every received frame |
| **No heap allocator on bare metal** | Fixed-size buffers only |
| **No RTOS thread isolation** | All processing in a single main loop |
| **Binary on-air format** | Frames must be decoded and classified efficiently |

Writing this logic in plain C leads to deeply nested `if/else` trees that are
hard to test and impossible to replay offline.

RIVR lets you describe the policy as a **declarative stream pipeline**:

```rivr
source rf_rx @lmp = rf_rx;

let chat = rf_rx
  |> filter.pkt_type(1)
  |> budget.toa_us(360000, 0.10, 360000)
  |> throttle.ticks(1);

emit { rf_tx(chat); }
```

This program is parsed, compiled to a DAG, and executed by the RIVR runtime —
either inside the ESP32 firmware (via a C FFI bridge) or on a host machine for
testing and replay.

---

## Repository layout

```
rivr/
├── rivr_core/          Rust library — parser, compiler, runtime
│   └── src/
│       ├── ast.rs          Abstract Syntax Tree
│       ├── parser.rs       Recursive-descent parser
│       ├── compiler.rs     AST → Node DAG compiler
│       ├── ffi.rs          C-ABI exports (#[no_mangle])
│       └── runtime/
│           ├── engine.rs   Scheduler + top-level run loop
│           ├── node.rs     All operator NodeKind variants + process()
│           ├── value.rs    Value enum (Int / Bool / Str / Bytes / Unit)
│           ├── event.rs    Event = Stamp + Value + tag + seq
│           ├── fixed.rs    FixedText<N> and FixedBytes<N> for no-alloc
│           └── opcode.rs   Compact OpCode enum (introspection)
├── rivr_host/          Rust binary — demos + Replay 2.0
│   └── src/
│       ├── main.rs         8 demos (window, filter, budget, merge, …)
│       └── replay.rs       Record / replay / assert trace engine
├── firmware_core/      C hardware layer (ESP32 + SX1262)
│   ├── main.c              app_main, main loop, RIVR_SIM_MODE
│   ├── radio_sx1262.c/.h   SX1262 LoRa driver
│   ├── timebase.c/.h       Monotonic + Lamport clocks
│   ├── dutycycle.c/.h      C-layer duty-cycle guard
│   ├── platform_esp32.c/.h GPIO / SPI / LED init
│   ├── ringbuf.h           Lock-free SPSC ring buffer
│   ├── protocol.c/.h       Binary on-air packet format (encode/decode/CRC)
│   └── routing.c/.h        Dedupe cache, TTL, neighbour table
└── rivr_layer/         RIVR ↔ firmware glue (C)
    ├── rivr_embed.c/.h     Engine init, rivr_tick(), rivr_register_sink()
    ├── rivr_sinks.c/.h     rf_tx, usb_print, log sinks
    ├── rivr_sources.c/.h   rf_rx, CLI, timer sources
    ├── rivr_cli.c/.h       Serial CLI chat interface (client builds only)
    └── rivr_programs/
        └── default_program.h  Built-in RIVR program string
```

---

## Key concepts

### Stamp

Every event carries a `Stamp { clock: u8, tick: u64 }`.
Two built-in clocks are defined:

| Clock id | Name  | Description |
|---|---|---|
| 0 | `mono` | Monotonic millisecond wall-clock (ESP32 timer) |
| 1 | `lmp`  | Lamport logical clock (LoRa mesh send/receive) |

Sources are annotated with `@clock_name`:

```rivr
source rf_rx @lmp  = rf_rx;   // events carry Stamp { clock=1 }
source usb   @mono = usb;     // events carry Stamp { clock=0 }
```

### Value

Each event's payload is one of:

| Variant | Rust type (alloc) | No-alloc type |
|---|---|---|
| `Int(i64)` | `i64` | `i64` |
| `Bool(bool)` | `bool` | `bool` |
| `Str(s)` | `String` | `FixedText<64>` |
| `Bytes(b)` | `Vec<u8>` | `FixedBytes<64>` |
| `Unit` | — | — |
| `Window(v)` | `Vec<String>` | *(alloc only)* |

### Pipeline (`|>`)

Operators are chained with the pipe operator:

```rivr
let clean = source |> op1 |> op2 |> op3;
```

Each operator transforms or drops events flowing through it.

### Emit

Processed events are delivered to **sinks**:

```rivr
emit {
  rf_tx(chat);
  usb_print(chat);
}
```

Sinks call back into C via `rivr_emit_dispatch()`.

---

## Components at a glance

| Component | Language | Role |
|---|---|---|
| `rivr_core` | Rust (`no_std + alloc`) | Language runtime — parse, compile, run |
| `rivr_host` | Rust (`std`) | Desktop demos, Replay 2.0, `rivrc` CLI |
| `firmware_core` | C | Hardware drivers, simulation mode, OLED display |
| `rivr_layer` | C | Glue between RIVR runtime and C firmware |
| Protocol / Routing | C | Binary packet format, mesh flooding |
| `tools/vscode-rivr` | JSON/TS | VS Code syntax highlighting + snippets |

---

## Quick start

```powershell
# Run all host demos (no hardware needed)
cargo run -p rivr_host

# Check / inspect a .rivr source file
cargo run -p rivr_host --bin rivrc -- my_program.rivr
cargo run -p rivr_host --bin rivrc -- --check my_program.rivr   # CI mode

# Build the Rust static library for ESP32
cargo build --features ffi

# See docs/en/build-guide.md for the full firmware build procedure
```
