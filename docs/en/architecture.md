# RIVR — Architecture

## System diagram

```
┌───────────────────────────────────────────────────────────────┐
│  ESP32 (Xtensa LX6)                                            │
│                                                               │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │  app_main() — main loop (1 ms yield)                      │ │
│  │                                                          │ │
│  │  radio_service_rx()  ← checks s_dio1_pending flag        │ │
│  │   └─ GetIrqStatus / ClearIrq / ReadBuffer                │ │
│  │      → rb_try_push(&rf_rx_ringbuf, frame)                │ │
│  │                                                          │ │
│  │  rivr_tick()                                             │ │
│  │   ├─ sources_rf_rx_drain()  ← rf_rx_ringbuf              │ │
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
│  DIO1 ISR (radio_isr): s_dio1_pending = true  ← flag only    │
│  SPI calls from ISR context are ILLEGAL on ESP32 (semaphores) │
│                                                               │
│  ┌─────────────┐     SPI     ┌─────────────────┐             │
│  │  ESP32 SoC  │ ──────────► │  E22-900M30S    │             │
│  │  DIO1 IRQ   │ ◄────────── │  SX1262 LoRa    │             │
│  └─────────────┘     8 MHz   └─────────────────┘             │
│  869.480 MHz · SF8 · BW125kHz · CR4/8 · +22dBm (~30dBm PA)  │
└───────────────────────────────────────────────────────────────┘
```

---

## Subsystem responsibilities

### 1. `firmware_core/` — Hardware layer (C)

| File | Responsibility |
|---|---|
| `main.c` | `app_main()`, main loop, simulation mode |
| `radio_sx1262.c` | SX1262 SPI driver; `radio_isr()` sets `s_dio1_pending` flag only (SPI is illegal from ISR); `radio_service_rx()` does all SPI from main loop |
| `timebase.c` | `tb_millis()` (mono clock), `tb_lmp_advance()` (Lamport) |
| `dutycycle.c` | Sliding-window duty-cycle tracker (C-layer guard) |
| `platform_esp32.c` | GPIO, SPI bus, LED initialisation |
| `ringbuf.h` | Lock-free SPSC ring buffer (ISR-safe) |
| `protocol.c` | Binary packet encode/decode, CRC-16/CCITT |
| `routing.c` | Dedupe cache (LRU ring), TTL decrement, neighbour table (with callsign) |
| `rivr_fabric.c` | Congestion-aware relay policy: 60 s sliding-window score, DELAY / DROP decisions for `PKT_CHAT` / `PKT_DATA` relay; enabled when `RIVR_FABRIC_REPEATER=1` |
| `display/` | SSD1306 128×64 OLED driver; I²C 400 kHz, horizontal addressing mode, single 1025-byte bulk flush per refresh; auto-detects I²C address 0x3C / 0x3D; 7-page rotating UI (overview, RF stats, routing, duty cycle, RIVR VM, neighbours, Fabric debug); runs as FreeRTOS task on CPU1 at priority 1; feature-gated by `FEATURE_DISPLAY=1` |

### 2. `rivr_layer/` — Glue layer (C)

| File | Responsibility |
|---|---|
| `rivr_embed.c` | `rivr_embed_init()`, `rivr_tick()`, emit dispatch wiring, NVS load/store, `rivr_embed_reload()` |
| `rivr_sources.c` | Drain ring-buffers → construct `rivr_event_t` → inject; multi-timer table (`sources_timer_drain`) |
| `rivr_sinks.c` | Receive emitted values → encode → hardware queues; `beacon_sink_cb` builds `PKT_BEACON` (callsign + net_id + hop_count) for `io.lora.beacon` |
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

## Node roles

RIVR nodes are configured at compile time via a variant header (`variants/<board>/config.h`).

| Role | Key macros | Relay behaviour |
|---|---|---|
| **Standard** (`esp32_hw`) | — | Full relay of all packet types |
| **Repeater** (`repeater_esp32devkit_e22_900`) | `RIVR_FABRIC_REPEATER=1`, `RIVR_BUILD_REPEATER=1` | Full relay; `PKT_CHAT` and `PKT_DATA` gated by Rivr Fabric congestion score |
| **Client** (`client_esp32devkit_e22_900`) | `RIVR_ROLE_CLIENT=1`, `RIVR_FABRIC_REPEATER=0` | Receives `PKT_CHAT`/`PKT_DATA` locally; does **not** relay them; control frames (`BEACON`, `ROUTE_REQ/RPL`, `ACK`, `PROG_PUSH`) relayed normally |

Node-role macros are wrapped in `#ifndef` inside the variant header, so
any `-D` build flag passed on the command line takes precedence.

---

## Data flow — received LoRa frame

```
SX1262 RxDone IRQ
       │
       ▼
 radio_isr()  [IRAM_ATTR]
 s_dio1_pending = true              ← flag only; SPI semaphores are illegal in ISR
       │
       ▼ (next main-loop iteration — before rivr_tick)
 radio_service_rx()
   GetIrqStatus(0x12) → ClearIrqStatus(0x02)
   GetRxBufferStatus(0x13) → ReadBuffer(0x1E)
   rb_try_push(&rf_rx_ringbuf, frame)
       │
       ▼
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
---

## Architecture hardening — what was added

This section documents the incremental hardening pass applied to the firmware.
All additions are backward-compatible: no wire format changes, no API breaks.

### Phase 1 — HAL abstraction layer (`firmware_core/hal/`)

| File | Purpose |
|---|---|
| `hal/radio_if.h` + `hal/radio_if.c` | Abstract radio vtable (`radio_if_vtable_t`). Concrete drivers register with `radio_if_register()`; callers use `radio_if_transmit()`, `radio_if_service_rx()`, etc. Existing `radio_sx1262_*` calls are unaffected — the HAL is an additive layer. |
| `hal/timer_if.h` | Thin inline wrappers around `tb_millis()` / `tb_lmp_now()` — `timer_if_millis()`, `timer_if_elapsed()`, `timer_if_expired()`. |
| `hal/crypto_if.h` | Unified crypto dispatch: `crypto_if_ed25519_verify()` (wraps existing `rivr_ed25519_verify()`); `crypto_if_psk_encrypt/decrypt()` (PSK-AES stubs, active when `RIVR_FEATURE_ENCRYPTION=1`). |
| `hal/feature_flags.h` | **Single source of truth** for all compile-time knobs (see table below). |

All existing direct calls to `radio_sx1262_*`, `tb_millis()`, etc. remain valid.
The HAL headers are available for new code and for future driver porting.

### Phase 2 — Consolidated feature flags (`hal/feature_flags.h`)

| Flag | Default | Description |
|------|---------|-------------|
| `RIVR_ROLE_CLIENT` | 0 | Client: TX/RX chat, no CHAT/DATA relay |
| `RIVR_ROLE_REPEATER` | 0 | Repeater: full relay including CHAT/DATA |
| `RIVR_FABRIC_REPEATER` | 0 | Congestion-aware relay suppression |
| `RIVR_RADIO_SX1262` | 1 | Use SX1262/E22 driver |
| `RIVR_RADIO_SX1276` | 0 | Use SX1276/RFM95 driver |
| `RIVR_FEATURE_ENCRYPTION` | 0 | PSK-AES-128-CTR frame encryption |
| `RIVR_FEATURE_COMPRESSION` | 0 | Reserved — future payload compression |
| `RIVR_FEATURE_BLE` | 0 | Reserved — future BLE provisioning |
| `RIVR_SIGNED_PROG` | 0 | OTA requires Ed25519 signature |
| `RIVR_SIM_MODE` | 0 | Host/sim build (no SPI/GPIO) |

Mutually-exclusive pairs (`ROLE_CLIENT` + `ROLE_REPEATER`, `SX1262` + `SX1276`)
produce a `#error` at compile time.

### Phase 3 — Protocol header (existing, no changes needed)

`protocol.h` already contains the full versioned wire header:
magic `0x5256`, `version=1`, `pkt_type`, `flags`, `ttl`, `hop`, `net_id`,
`src_id`, `dst_id`, `seq`, CRC-16/CCITT.  No wire-format changes were needed.

`capability_flags` and `feature_bitmap` are not added to the wire format
(would break backward compat).  They are encoded in the `@SUPPORTPACK` JSON
via `build_info_write_supportpack()` for host-side introspection.

### Phase 4 — Duty-cycle metrics (added to `rivr_metrics_t`)

Two named fields added to `rivr_metrics_t` in `rivr_metrics.h`:

| Field | Description |
|-------|-------------|
| `rf_airtime_ms_total` | Cumulative TX time-on-air in milliseconds since boot |
| `rf_duty_blocked_total` | Total TX attempts blocked by DC or token-bucket gate |

These aggregate across both the hardware DC gate (`dutycycle_check()`) and
the class-based token-bucket gate (`airtime_sched_check()`).

### Phase 5 — Extended logging (`rivr_log.h`)

`rivr_log.h` now provides five severity levels:

| Macro | Level | Compile-time default |
|-------|-------|----------------------|
| `RIVR_LOGT(tag, ...)` | TRACE (0) | stripped unless `-DRIVR_LOG_LEVEL=0` |
| `RIVR_LOGD(tag, ...)` | DEBUG (1) | stripped unless `≤1` |
| `RIVR_LOGI(tag, ...)` | INFO  (2) | active (default) |
| `RIVR_LOGW(tag, ...)` | WARN  (3) | active |
| `RIVR_LOGE(tag, ...)` | ERROR (4) | active |

Set `-D RIVR_LOG_LEVEL=5` in production `build_flags` for SILENT mode —
all log strings are removed from the binary, reducing flash by ~2-4 KB.

The runtime `g_rivr_log_mode` variable (controllable via `rivr_cli`) still
allows toggling INFO output at runtime without a recompile.

### Phase 6 — Watchdog & crash marker (`rivr_panic.h/.c`)

The IDF Task Watchdog was already present (`esp_task_wdt_add/reset()`).
Added:

- **`rivr_panic_mark(reason)`** — writes a 16-byte crash record to
  `RTC_DATA_ATTR` slow memory (survives warm/WDT reset).
- **`rivr_panic_check_prev()`** — called at the start of `app_main()`;
  reads IDF `esp_reset_reason()` + the crash marker and emits:
  ```
  @CRASH {"reason":"TASK_WDT","uptime_ms":87432,"resets":3,"idf_reason":8}
  ```
- **`rivr_panic_clear_reset_count()`** — called after 60 s of clean
  uptime to reset the consecutive-crash counter.

Memory cost: 16 bytes RTC slow memory, ~200 bytes flash.

### Phase 7 — Routing (existing, no changes needed)

`routing.h/.c` already implements:
- Flood with TTL/dedupe hardening
- SNR-weighted neighbour table
- Hybrid unicast (ROUTE_REQ/ROUTE_RPL)
- ETX-equivalent scoring via EWMA RSSI + SNR
- Per-packet-type forward budget

No separate `flood.c` / `opportunistic.c` split was performed — the single
`routing.c` module is cohesive and splitting it would only move code without
adding capability.

### Phase 8 — Crypto layer (`hal/crypto_if.h`)

Ed25519 OTA signature verification was already in place (`rivr_ota.c`).
Added in `hal/crypto_if.h`:

- `crypto_if_ed25519_verify()` — thin wrapper around `rivr_ed25519_verify()`.
- `crypto_if_psk_encrypt/decrypt()` stubs — compile to no-ops when
  `RIVR_FEATURE_ENCRYPTION=0` (default).  When `=1`, the caller provides
  `RIVR_PSK_HEX` key; nonce is written as a 4-byte prefix to the payload.

To enable frame-level PSK encryption for a production deployment:
```ini
; platformio.ini
build_flags =
    -D RIVR_FEATURE_ENCRYPTION=1
    -D RIVR_PSK_HEX=\"0102030405060708090a0b0c0d0e0f10\"
```

---

## Memory impact summary

| Change | RAM delta | Flash delta |
|--------|-----------|-------------|
| `hal/radio_if.c` (vtable pointer) | +4 bytes | +48 bytes |
| `rivr_panic.c` (RTC crash marker) | +16 bytes RTC | ~200 bytes |
| `rivr_metrics_t` (2 new fields) | +8 bytes | +60 bytes |
| `rivr_log.h` (compile-time strip) | 0 | 0 (or −2-4 KB at SILENT) |
| `hal/*.h` (headers, no .c) | 0 | 0 |
| **Total** | **+28 bytes (+16 RTC)** | **~+308 bytes** |

Measured firmware sizes after hardening:
- **client_esp32devkit_e22_900**: RAM 11.5% (37,772 / 327,680 B), Flash 31.0% (609,385 / 1,966,080 B)
- **repeater_esp32devkit_e22_900**: RAM 11.7% (38,276 / 327,680 B), Flash 30.0% (590,801 / 1,966,080 B)