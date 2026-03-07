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
| `routing.c` | Dedupe cache (LRU ring), TTL decrement, jitter, forward-budget caps, loop-guard relay fingerprint, `pkt_id` split; `routing_next_hop_score()` computes composite RSSI+SNR score for a candidate next-hop peer |
| `route_cache.c` | Unicast reverse-path cache; `rivr_route_t` (dest, next_hop, metric, hop_count, expires_ms); `route_cache_best_hop()` three-tier next-hop decision: (1) best scored cache entry, (2) `neighbor_best()` direct peer fallback, (3) return false → caller floods; `entry_composite_score()`: metric × hop-weight × age-decay × loss-penalty |
| `neighbor_table.c` | 16-slot BSS link-quality table; `rivr_neighbor_t` tracks EWMA RSSI/SNR (`rssi_avg`, `snr_avg`), seq-gap loss-rate (`loss_rate`), `last_seen_ms`, and `flags` (`NTABLE_FLAG_DIRECT`, `NTABLE_FLAG_STALE`, `NTABLE_FLAG_BEACON`); API: `neighbor_update()`, `neighbor_find()`, `neighbor_best()`, `neighbor_link_score()`, `neighbor_set_flag()`, `neighbor_table_expire()`; `g_ntable` global exposed via `rivr_embed.h` |
| `rivr_fabric.c` | Congestion-aware relay policy: 60 s sliding-window score, DELAY / DROP decisions for `PKT_CHAT` / `PKT_DATA` relay; enabled when `RIVR_FABRIC_REPEATER=1` |
| `rivr_policy.c` | Runtime-adjustable policy: `rivr_policy_params_t` (beacon interval, TX power, relay throttle, node role); `@PARAMS` parser + NVS-backed storage; `@POLICY` JSON reporter; USB origination gate (`rivr_policy_allow_origination()`); HMAC-SHA-256 signature verification (`rivr_verify_params_sig()`); `policy` CLI command; metrics counters (`params_sig_ok_count`, `params_sig_fail_count`); built-in selftest |
| `rivr_ota.c` | Signed `PKT_PROG_PUSH` gate: Ed25519 verify (`rivr_ota_verify()`), anti-replay sequence counter stored in NVS, `rivr_ota_activate()` / `rivr_ota_confirm()` / `rivr_ota_is_pending()` |
| `crypto/hmac_sha256.c` | Self-contained FIPS 180-4 SHA-256 + RFC 2104 HMAC-SHA-256; no heap allocation; stack cost ≈460 B; tested against RFC 4231 TC1 known vector |
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
| `RIVR_FEATURE_SIGNED_PARAMS` | 0 | `@PARAMS` update requires HMAC-SHA-256 MAC in `sig=` field |
| `RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS` | 1 | Accept unsigned `@PARAMS` when `SIGNED_PARAMS=0` (dev grace period) |
| `RIVR_PARAMS_PSK_HEX` | *64 zeros* | 32-byte PSK encoded as 64 hex chars; override for production deployments |

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

---

### Phase 9 — Policy engine (`rivr_policy.h/.c`)

A new runtime-adjustable policy layer sits between the mesh transport and the
RIVR engine.  All changes are **backward-compatible**: no wire-format changes,
no new packet types, zero cost when every feature flag is at its default.

#### `@PARAMS` — OTA parameter updates

A privileged node broadcasts a `PKT_PROG_PUSH` frame whose text payload begins
with `@PARAMS`.  Receiving nodes parse the key/value pairs, validate them
against compile-time bounds, persist them to NVS, and hot-apply them without a
reboot:

```
@PARAMS beacon=30000 txpow=14 throttle=500 role=client sig=<64hex>
```

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `beacon` | 30 000 ms | 1 000 – 300 000 | Beacon interval |
| `txpow` | 22 dBm | 2 – 22 | LoRa TX power |
| `throttle` | 0 ms | 0 – 60 000 | Minimum gap between relay TX (0 = unlimited) |
| `role` | 0 (standard) | 0–3 | 0=standard 1=client 2=repeater 3=gateway |
| `sig` | *(optional)* | 64 hex chars | HMAC-SHA-256(PSK, bytes before ` sig=`) |

Nodes respond with `@PARAMS ACK` on acceptance or the appropriate rejection
message on failure.

#### Role-based relay throttle

`rivr_node_role_t` is stored as `RIVR_PARAM_ID_ROLE` (param index 5) in the
policy parameter block.  When `role == RIVR_NODE_ROLE_CLIENT`, the relay
throttle interval is doubled automatically at the policy layer before being
applied to the routing engine, reducing traffic from leaf nodes.

#### USB origination gate

`rivr_policy_allow_origination()` returns `false` when the node's accumulated
airtime since last `@PARAMS reset` exceeds the configured origination budget.
When blocked, `rivr_sources.c` emits:

```
@DROP {"reason":"origination_budget","src":"usb"}
```

#### `@POLICY` response

The `policy` CLI command (client builds) or any `@POLICY?` mesh query triggers
a JSON status line:

```json
@POLICY {"beacon":30000,"txpow":22,"throttle":0,"role":0,
         "relayed":142,"dropped":3,"sig_ok":12,"sig_fail":0}
```

#### HMAC-SHA-256 signed `@PARAMS` (optional)

Enable with `-DRIVR_FEATURE_SIGNED_PARAMS=1` in `platformio.ini`:

```ini
build_flags =
    -D RIVR_FEATURE_SIGNED_PARAMS=1
    -D RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS=0
    -D RIVR_PARAMS_PSK_HEX='\"0102...1e1f\"'   ; 32-byte key as 64 hex chars
```

Generate a signed `@PARAMS` string with openssl:

```bash
MSG='@PARAMS beacon=30000 txpow=14'
SIG=$(printf '%s' "$MSG" | openssl dgst -sha256 -hmac "$(xxd -r -p <<<"$PSK_HEX")" -binary | xxd -p -c 256)
echo "$MSG sig=$SIG"
```

The default PSK is 32 zero bytes — **change it for any production deployment.**

#### Memory impact (policy engine)

| Change | RAM delta | Flash delta |
|--------|-----------|-------------|
| `rivr_policy.c` (params + metrics) | +48 bytes BSS | ~1.8 KB |
| `crypto/hmac_sha256.c` | 0 (stack only) | ~1.4 KB |
| `rivr_sources.c` sig gate | 0 | ~80 bytes |

---

### Phase 10 — Loop-guard relay fingerprint

#### Problem

A buggy or deliberately mutated repeater firmware can create a forwarding loop
that standard dedupe cannot stop: the repeater increments (or randomises) the
`seq` field before re-broadcasting, so each traversal looks like a fresh packet
to every node's dedupe cache.  Left unchecked, this produces a **packet storm**
that saturates the LoRa duty-cycle budget and prevents legitimate traffic.

#### Wire-format change

A single byte (`loop_guard`) is appended to the packet header at offset **[22]**,
growing `RIVR_PKT_HDR_LEN` from 22 to **23**.  The minimum frame size increases
from 24 to **25** bytes; the maximum application payload shrinks from 231 to
**230** bytes.

```
Byte  Field
 0    magic
 1    version
 2    pkt_type
 3    flags
 4–7  src_id
 8–11 dst_id
12–15 seq
16    ttl
17    hop
18    payload_len
19–21 (reserved / callout fields)
22    loop_guard          ← NEW: OR-accumulating relay fingerprint
23+   payload
end   CRC-16/CCITT (2 bytes)
```

#### Algorithm

Each relay node computes a single-byte fingerprint of its own node ID:

```c
static inline uint8_t routing_loop_guard_hash(uint32_t node_id) {
    uint8_t h = (node_id & 0xFF) ^ ((node_id >> 8) & 0xFF)
              ^ ((node_id >> 16) & 0xFF) ^ ((node_id >> 24) & 0xFF);
    return (h != 0u) ? h : 1u;   /* never 0 */
}
```

Before forwarding, `routing_flood_forward()` (step 3, after dedupe and TTL):

1. Compute `my_h = routing_loop_guard_hash(my_id)`.
2. If `(pkt->loop_guard & my_h) == my_h` → **drop** (`RIVR_FWD_DROP_LOOP`).
   Increment `g_rivr_metrics.loop_detect_drop`.
3. Otherwise: `pkt->loop_guard |= my_h`, then forward normally.

Originators always set `loop_guard = 0` (including `routing_build_route_req()`).
Passing `my_id = 0` to `routing_flood_forward()` skips the loop check entirely
(used by the fuzz harness and replay tool where the local node ID is unknown).

#### Properties

| Property | Value |
|----------|-------|
| Wire overhead | +1 byte per packet |
| False-positive rate (4-hop path) | ~2.5% |
| False-positive rate (7-hop path) | ~8% |
| Heap allocation | none |
| Crypto | none |
| Defeats mutated-seq storms | yes |

False positives (incorrectly dropping a non-looping packet) are possible when
two nodes share a bit in their fingerprint.  They are acceptable here because:
(a) the storm-defence benefit outweighs the rare false drop, and (b) the
originator will retransmit on its normal schedule.

#### Metrics and logging

- **`loop_detect_drop`** field added to `rivr_metrics_t`; emitted in `@MET`
  JSON as `"loop_drop"`.
- `RIVR_LOGW` in `rivr_sources.c` on every loop drop:
  `rf_rx: LOOP-DROP src=0x%08lx seq=%u pkt_id=%u guard=0x%02x`.

#### Check order

`routing_flood_forward()` evaluates conditions in this order, ensuring existing
safety properties are not weakened:

1. **Dedupe** — `(src_id, pkt_id)` already seen → `RIVR_FWD_DROP_DEDUPE`
2. **TTL** — `ttl == 0` → `RIVR_FWD_DROP_TTL`
3. **Loop guard** — fingerprint match → `RIVR_FWD_DROP_LOOP` *(new)*
4. **Budget** — airtime budget exhausted → `RIVR_FWD_DROP_BUDGET`
5. **Mutate and forward** — decrement TTL, increment hop, set `PKT_FLAG_RELAY`,
   OR-in `my_h` to `loop_guard`, enqueue for TX.

#### Memory impact (loop guard)

| Change | RAM delta | Flash delta |
|--------|-----------|-------------|
| `loop_guard` wire byte | 0 (no heap) | 0 |
| `loop_detect_drop` metric field | +4 bytes BSS | ~40 bytes |
| `routing_loop_guard_hash()` (inline) | 0 | 0 |
| Mutate step in `routing_flood_forward()` | 0 | ~80 bytes |
| **Total** | **+4 bytes** | **~+120 bytes** |

---

### Phase 11 — Packet identity split (`seq` u16 + `pkt_id` u16)

#### Problem

The original `seq` field was a single u32 serving three unrelated roles:

1. **Application sequence** — message ordering, Lamport clock hint.
2. **Forwarding identity** — dedupe key for `(src_id, seq)`.
3. **Control-plane correlation** — ROUTE_REQ / ROUTE_RPL token.

Overloading one field caused a critical correctness issue: a **fallback flood**
(re-injected after unicast route failure) must carry the same `seq` so
application-level ordering is preserved, yet requires a *different* forwarding
identity so nodes that already deduped the original unicast will forward it.
There was no way to satisfy both constraints with a single field.

#### Wire-format change (zero overhead)

The existing u32 `seq` at bytes **[17–20]** is split into two u16 fields that
occupy exactly the same four bytes — no header length change.

```
Before (Phase ≤ 10):                After (Phase 11):
  [17–20]  seq  u32 LE               [17–18]  seq     u16 LE
                                     [19–20]  pkt_id  u16 LE
```

Complete current header map:

| Bytes | Field         | Type    | Notes |
|-------|---------------|---------|-------|
| 0–1   | `magic`       | u16 LE  | `0x5256` ("RV") |
| 2     | `version`     | u8      | `RIVR_PROTO_VER = 1` |
| 3     | `pkt_type`    | u8      | `PKT_*` constants |
| 4     | `flags`       | u8      | `PKT_FLAG_*` bitmask |
| 5     | `ttl`         | u8      | Decremented per hop |
| 6     | `hop`         | u8      | Incremented per hop |
| 7–8   | `net_id`      | u16 LE  | Network / channel discriminator |
| 9–12  | `src_id`      | u32 LE  | Sender node ID |
| 13–16 | `dst_id`      | u32 LE  | Destination (0 = broadcast) |
| 17–18 | `seq`         | u16 LE  | **App sequence** — ordering, Lamport hint, control-plane correlation |
| 19–20 | `pkt_id`      | u16 LE  | **Forwarding identity** — unique per wire injection; dedupe keys on `(src_id, pkt_id)` |
| 21    | `payload_len` | u8      | Payload byte count |
| 22    | `loop_guard`  | u8      | OR-accumulating relay fingerprint (Phase 10) |
| 23+   | payload       | bytes   | Application payload |
| end   | CRC           | u16 LE  | CRC-16/CCITT |

#### Field responsibilities

| Field    | Relayed unchanged | Changed on | Purpose |
|----------|--------------------|------------|---------|
| `seq`    | Yes | New origination only | Message ordering, control-plane correlation, Lamport hint |
| `pkt_id` | Yes | Each wire injection (incl. retransmits, fallback floods) | Dedupe key `(src_id, pkt_id)`; jitter seed |

#### Behavioral improvements

| Scenario | Before | After |
|----------|--------|-------|
| Fallback flood | Must change `seq` (breaks ordering) or keep it (dedupe-dropped) | Keep `seq`, bump `pkt_id` — ordering preserved AND not deduped |
| Retransmit | Same `seq` — correctly deduped | Same `pkt_id` — correctly deduped |
| Jitter independence | Fallback flood shares jitter seed with original | Fresh `pkt_id` → different jitter → independent spreading |
| Control-plane match | `seq` is correlation token | `seq` is token; `pkt_id = ++g_ctrl_seq` → each injection unique |

#### API changes

| Function | Old signature | New signature |
|----------|---------------|---------------|
| `routing_build_route_req()` | `uint32_t seq, ...` | `uint16_t seq, uint16_t pkt_id, ...` |
| `routing_build_route_rpl()` | `uint32_t seq, ...` | `uint16_t seq, uint16_t pkt_id, ...` |
| `routing_jitter_ticks()` | `uint32_t seq` | `uint16_t pkt_id` |
| `routing_forward_delay_ms()` | `uint32_t seq` | `uint16_t pkt_id` |
| `routing_dedupe_check()` | `uint32_t seq` | `uint16_t pkt_id` |

All callers updated: `rivr_sources.c`, `rivr_sinks.c`, `rivr_cli.c`,
`firmware_core/main.c`, `tests/test_acceptance.c`, `tests/replay.c`
(backward-compat: traces without `pkt_id` default to `pkt_id = seq`),
`tests/fuzz_ffi_harness.c`.

#### Properties

| Property | Value |
|----------|-------|
| Wire overhead | **zero** (same 23-byte header, same positions) |
| Heap delta | 0 |
| RAM delta | 0 (`dedupe_entry_t` field rename only, same struct size) |
| Flash delta | ~+60 bytes (encode/decode split) |
| Wire backward compatibility | Old receivers read `seq` as the 32-bit union of both fields; new `seq` = low 16 bits, `pkt_id` = high 16 bits — no crash, just `pkt_id` silently lost on old firmware |

---

### Phase 12 — Neighbor table + neighbor-aware next-hop routing

#### Neighbor table (`firmware_core/neighbor_table.h/.c`)

A new 16-slot BSS table (`rivr_neighbor_table_t`) tracks link quality to all
directly heard peers without heap allocation.

##### Data structures

```c
#define NTABLE_SIZE             16      /* max tracked peers           */
#define NTABLE_EXPIRY_MS    120000      /* stale → evict after 2 min   */
#define NTABLE_STALE_MS      30000      /* mark STALE after 30 s quiet */
#define NTABLE_SCORE_UNICAST_MIN 20     /* min score to attempt unicast */

#define NTABLE_FLAG_DIRECT  0x01       /* packet received directly (hop==1) */
#define NTABLE_FLAG_STALE   0x02       /* no frame for > NTABLE_STALE_MS   */
#define NTABLE_FLAG_BEACON  0x04       /* last-heard frame was PKT_BEACON  */

typedef struct {
    uint32_t neighbor_id;   /* node ID of the peer                    */
    int16_t  rssi_avg;      /* EWMA smoothed RSSI (dBm × 10)          */
    int8_t   snr_avg;       /* EWMA smoothed SNR (dB)                 */
    uint8_t  loss_rate;     /* seq-gap loss estimate, 0-255 (0=clean) */
    uint32_t last_seen_ms;  /* tb_millis() of most recent RX          */
    uint8_t  flags;         /* NTABLE_FLAG_* bitmask                  */
} rivr_neighbor_t;
```

##### API

```c
void              neighbor_table_init(rivr_neighbor_table_t *tbl);
rivr_neighbor_t  *neighbor_update(rivr_neighbor_table_t *tbl,
                                  uint32_t id, int16_t rssi, int8_t snr,
                                  uint16_t seq, uint32_t now_ms, uint8_t flags);
const rivr_neighbor_t *neighbor_find(const rivr_neighbor_table_t *tbl, uint32_t id);
const rivr_neighbor_t *neighbor_best(const rivr_neighbor_table_t *tbl, uint32_t now_ms);
uint8_t           neighbor_link_score(const rivr_neighbor_t *n);
void              neighbor_set_flag(rivr_neighbor_table_t *tbl, uint32_t id, uint8_t flag);
uint8_t           neighbor_table_expire(rivr_neighbor_table_t *tbl, uint32_t now_ms);
```

Call site: `rivr_sources.c` calls `neighbor_update()` on every decoded RX
frame; the display driver reads the table directly via `g_ntable`.

#### Neighbor-aware next-hop routing (`firmware_core/route_cache.h/.c`)

`route_cache_best_hop()` replaces the previous `route_cache_tx_decide()` call
in `rivr_sinks.c` with a three-tier decision:

| Tier | Condition | Action |
|------|-----------|--------|
| 1 | Route cache has entry for `dst_id` | Pick highest `entry_composite_score()` |
| 2 | No cache hit; `neighbor_best()` score ≥ `NTABLE_SCORE_UNICAST_MIN` | Promote direct peer as next-hop |
| 3 | No viable hop found | Return `false` → caller floods |

`entry_composite_score()` weights four factors:

```
score = metric × hop_weight(100%/75%/50%) × age_decay × (1 − loss_rate)
```

New fields in `rivr_route_t`:

```c
typedef struct {
    uint32_t dest_id;      /* destination node ID          */
    uint32_t next_hop_id;  /* first-hop node ID            */
    uint16_t metric;       /* composite link-quality score */
    uint8_t  hop_count;    /* path length in hops          */
    uint32_t expires_ms;   /* tb_millis() expiry timestamp */
} rivr_route_t;
```

#### `routing_next_hop_score()` (`firmware_core/routing.h/.c`)

New function exposed by `routing.h` that computes the composite next-hop
desirability score for a given candidate peer ID:

```c
uint8_t routing_next_hop_score(const rivr_neighbor_table_t *ntbl,
                               uint32_t candidate_id,
                               uint32_t now_ms);
```

Used internally by `route_cache_best_hop()` and available to any caller
that wants to compare peer quality without routing a packet.

#### New metrics

| Field | `@MET` key | Meaning |
|-------|-----------|--------|
| `neighbor_route_used_total` | `nb_route_ok` | Unicast attempted with neighbor-quality best-hop |
| `neighbor_route_failed_total` | `nb_route_fail` | Best-hop score=0 → fell back to flood |

#### Memory impact (neighbor table + routing)

| Change | RAM delta | Flash delta |
|--------|-----------|-------------|
| `rivr_neighbor_table_t` (16 entries × ~14 B) | +224 bytes BSS | — |
| `rivr_route_t` extension (2 new fields) | +0 (inline in existing cache) | ~+40 bytes |
| `routing_next_hop_score()` | 0 | ~+60 bytes |
| `route_cache_best_hop()` + score fn | 0 | ~+200 bytes |
| 2 new metrics fields | +8 bytes | +60 bytes |
| **Total** | **+232 bytes** | **~+360 bytes** |