# RIVR — Architecture

## System diagram

```
┌───────────────────────────────────────────────────────────────┐
│  ESP32 (Xtensa LX6)                                            │
│                                                               │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │  app_main() — main loop (1 ms yield)                      │ │
│  │                                                          │ │
│  │  rivr_tick()                                             │ │
│  │   ├─ sources_rf_rx_drain()  ← ISR → rf_rx_ringbuf        │ │
│  │   │      protocol_decode() → Value::Bytes               │ │
│  │   │      rivr_inject_event("rf_rx", event)              │ │
  │   ├─ sources_cli_drain()                                 │ │
  │   ├─ sources_timer_drain()  ← rivr_timer_entry_t[8]     │ │
  │   │      fire due timers → Value::Int(mono_ms)          │ │
  │   └─ rivr_engine_run(max_steps)                          │ │
  │         Engine DAG processes events                     │ │
  │         → rivr_emit_dispatch() callbacks                │ │
  │              ├─ rf_tx_sink_cb()    → rf_tx_queue        │ │
  │              ├─ beacon_sink_cb()   → rf_tx_queue        │ │
│  │              └─ usb_print_sink_cb() → printf            │ │
│  │                                                          │ │
│  │  tx_drain_loop()                                         │ │
│  │   └─ rb_pop(rf_tx_queue) → dutycycle_check() → TX        │ │
│  └──────────────────────────────────────────────────────────┘ │
│                                                               │
│  ┌─────────────┐     SPI     ┌─────────────────┐             │
│  │  ESP32 SoC  │ ──────────► │  SX1262 LoRa    │             │
│  │  DIO1 ISR   │ ◄────────── │  RxDone / TxDone│             │
│  └─────────────┘             └─────────────────┘             │
└───────────────────────────────────────────────────────────────┘
```

---

## Subsystem responsibilities

### 1. `firmware_core/` — Hardware layer (C)

| File | Responsibility |
|---|---|
| `main.c` | `app_main()`, main loop, simulation mode |
| `radio_sx1262.c` | SX1262 SPI driver, ISR, ring-buffer management |
| `timebase.c` | `tb_millis()` (mono clock), `tb_lmp_advance()` (Lamport) |
| `dutycycle.c` | Sliding-window duty-cycle tracker (C-layer guard) |
| `platform_esp32.c` | GPIO, SPI bus, LED initialisation |
| `ringbuf.h` | Lock-free SPSC ring buffer (ISR-safe) |
| `protocol.c` | Binary packet encode/decode, CRC-16/CCITT |
| `routing.c` | Dedupe cache (LRU ring), TTL decrement, neighbour table |

### 2. `rivr_layer/` — Glue layer (C)

| File | Responsibility |
|---|---|
| `rivr_embed.c` | `rivr_embed_init()`, `rivr_tick()`, emit dispatch wiring, NVS load/store, `rivr_embed_reload()` |
| `rivr_sources.c` | Drain ring-buffers → construct `rivr_event_t` → inject; multi-timer table (`sources_timer_drain`) |
| `rivr_sinks.c` | Receive emitted values → encode → hardware queues; `beacon_sink_cb` for `io.lora.beacon` |
| `default_program.h` | Built-in RIVR program strings (`RIVR_DEFAULT_PROGRAM`, `RIVR_BEACON_PROGRAM`, `RIVR_MESH_PROGRAM`) |

### 3. `rivr_core/` — Language runtime (Rust)

| Module | Responsibility |
|---|---|
| `parser.rs` | Recursive-descent parser: RIVR text → `Program` AST |
| `compiler.rs` | `Program` AST → `Engine` DAG (nodes + edges) |
| `runtime/engine.rs` | Scheduler: run nodes in topological order, clock dispatch |
| `runtime/node.rs` | All operator `NodeKind` variants + `process()` logic |
| `runtime/value.rs` | `Value` enum, `StrBuf`/`ByteBuf` type aliases |
| `runtime/event.rs` | `Event` struct: `Stamp` + `Value` + optional tag + seq |
| `runtime/fixed.rs` | `FixedText<N>`, `FixedBytes<N>` (no-alloc heap alternatives) |
| `runtime/opcode.rs` | `OpCode` enum — compact instruction-set tag |
| `ffi.rs` | `#[no_mangle]` C-ABI exports, `ENGINE_SLOT` static |

### 4. `rivr_host/` — Desktop tooling (Rust)

| File | Responsibility |
|---|---|
| `main.rs` | 8 interactive demos for each operator category |
| `replay.rs` | Record / replay / assert JSONL trace streams |
| `bin/rivrc.rs` | `rivrc` CLI — parse + compile a `.rivr` file, print node graph; `--check` for CI |

---

## Data flow — received LoRa frame

```
SX1262 RxDone IRQ
       │
       ▼
 radio_isr()
 rb_try_push(&rf_rx_ringbuf, frame)       ← ISR, no RIVR, no alloc
       │
       ▼ (next rivr_tick)
 sources_rf_rx_drain()
   protocol_decode(frame.data, frame.len)  ← validate magic + CRC
   tb_lmp_advance(seq & 0xFFFF)            ← advance Lamport clock
   rivr_inject_event("rf_rx", event)       ← Value::Bytes(raw frame)
       │
       ▼
 rivr_engine_run()
   Engine::inject("rf_rx", event)
       │ (topological pass)
   filter.pkt_type(1)  →  pkt_type byte [3] == 1 ?
       │ pass
   budget.toa_us(360000, 0.10, 360000)  →  within duty budget ?
       │ pass
   throttle.ticks(1)  →  first event this Lamport tick ?
       │ pass
   NodeKind::Emit { sink: LoraTx }
   rivr_emit_dispatch("rf_tx", value)
       │
       ▼
 rf_tx_sink_cb()
   protocol_decode(v.as_bytes) → build rf_tx_request_t
   rb_try_push(&rf_tx_queue, req)
       │
       ▼ (tx_drain_loop)
 dutycycle_check()  →  C-layer guard
 radio_transmit()   →  SPI → SX1262
```

---

## Clocks and time domains

```
clock 0 (mono)  — tb_millis()
  └─ Used by: sensors, CLI, window.ms, throttle.ms, debounce.ms
  └─ Unit: milliseconds

clock 1 (lmp)   — tb_lmp_advance() / tb_lmp_now()
  └─ Used by: LoRa RX events, mesh protocol ticks
  └─ Unit: Lamport ticks (monotonically increasing per receive)
  └─ Purpose: maintains causal ordering across nodes without GPS sync
```

The `@clock_name` annotation on a source determines which clock domain
all tick-based operators (`window.ticks`, `throttle.ticks`, etc.) apply
their time constraints in.

---

## Memory model

| Location | Size | Content |
|---|---|---|
| BSS (Rust `static mut`) | ~16 KB | `ENGINE_SLOT: MaybeUninit<Engine>` |
| BSS (C) | configurable | `rf_rx_ringbuf`, `rf_tx_queue` ring buffers |
| BSS (C) | 2 KB | `s_nvs_program[2048]` — NVS-loaded program text |
| Stack (C) | per-call | Decoded frames, `rivr_event_t` temporaries |
| Flash (C string) | ~512 bytes | Compiled-in RIVR program strings |
| NVS partition | up to 2 KB | User-pushed RIVR program (`rivr/program`) |

**No heap allocation** occurs after `app_main()` initialisation. The Engine
is initialised once via `rivr_engine_init()` — either from NVS or the compiled-in
default — and can be hot-reloaded at runtime via `rivr_embed_reload()` when
a `PKT_PROG_PUSH` frame arrives.

---

## Feature flags (`rivr_core`)

| Flag | Effect |
|---|---|
| `std` *(default)* | Enables `std`, heap (`Vec`/`String`), `serde_json` |
| `alloc` | Heap types without `std` (e.g. RTOS targets with allocator) |
| `ffi` | Compiles `ffi.rs` and exposes `#[no_mangle]` C exports |
| `embedded` | Alias for `ffi` + `--no-default-features` |
| *(none)* | Pure no-heap: `FixedText<64>` / `FixedBytes<64>` |

---

## Build artefacts

| Build command | Output |
|---|---|
| `cargo build` | `target/debug/librivr_core.rlib` (host library) |
| `cargo build --features ffi` | `target/debug/librivr_core.a` (C-linkable) |
| `cargo build --features ffi --release` | `target/release/librivr_core.a` |
| `cargo +esp build --target xtensa-esp32-espidf --features ffi` | Xtensa cross-compiled `.a` |

`main/CMakeLists.txt` automatically picks the release library if present,
falling back to the debug library.
