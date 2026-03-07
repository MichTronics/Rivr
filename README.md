# RIVR — Reactive LoRa Mesh Runtime

> A small reactive programming language and deterministic runtime for off-grid LoRa mesh
> networks.  Runs on ESP32 + SX1262/SX1276 with **zero heap allocation after boot**.

[![CI](https://github.com/your-org/Rivr/actions/workflows/ci.yml/badge.svg)](https://github.com/your-org/Rivr/actions/workflows/ci.yml)

---

## What is RIVR?

RIVR lets you describe a **data-flow pipeline** over radio packets in a small DSL, then compiles
and runs that pipeline inside an ESP32 firmware — no operating system, no garbage collector.

```rivr
source rf_rx @lmp = rf;

let chat = rf_rx
  |> filter.pkt_type(1)             -- PKT_CHAT only
  |> budget.toa_us(300000, 0.10, 280000)  -- EU868 g3 duty-cycle guard
  |> throttle.ticks(1);            -- max once per Lamport tick

let alerts = rf_rx
  |> filter.pkt_type(10)            -- PKT_ALERT: priority, 20 % budget
  |> budget.toa_us(300000, 0.20, 280000);

emit { io.lora.tx(chat);   }      -- rebroadcast chat over LoRa
emit { io.lora.tx(alerts); }      -- forward alerts immediately
```

Write the pipeline once.  RIVR parses and compiles it on-device at boot; the engine
evaluates it on every incoming frame with bounded worst-case latency.

---

## Why is RIVR different?

| | RIVR | GPIO-level C | MQTT bridge |
|---|---|---|---|
| **Memory model** | Zero heap after boot | Static / heap mix | Heap-heavy |
| **Duty-cycle safety** | Built-in sliding-window limiter | Hand-rolled or absent | N/A |
| **OTA program update** | `PKT_PROG_PUSH` over mesh | Requires full reflash | Topic reconfiguration |
| **Stream composition** | First-class `filter / window / merge` | Ad-hoc if-else chains | Broker rules engine |
| **Target** | ESP32 bare-metal (no OS) | Any MCU | Requires IP stack |

---

## Quickstart — host simulation

No hardware needed.

```bash
# 1. Install Rust (if not present)
curl https://sh.rustup.rs -sSf | sh

# 2. Build the host tools
cargo build -p rivr_host

# 3. Compile and inspect a RIVR program
cargo run -p rivr_host --bin rivrc -- rivr_replay.jsonl

# 4. Replay captured traffic through the engine
cargo run -p rivr_host -- rivr_replay.jsonl
```

See [docs/en/build-guide.md](docs/en/build-guide.md) for the full toolchain setup.

---

## Flash firmware

### ESP32 DevKit + E22-900M30S (SX1262)

```bash
# Client node (send + receive chat, no relay)
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900 -t upload

# Dedicated repeater (Rivr Fabric relay suppression enabled)
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900 -t upload
```

### LilyGo LoRa32 v2.1 (SX1276, onboard OLED)

```bash
~/.platformio/penv/bin/pio run -e client_lilygo_lora32_v21 -t upload
```

Monitor at 115200 baud:

```bash
~/.platformio/penv/bin/pio device monitor
```

Default air parameters: **869.480 MHz · SF8 · BW 125 kHz · CR 4/8 · preamble 8**.
Override frequency at build time: `-DRIVR_RF_FREQ_HZ=915000000`.

---

## RIVR DSL in 60 seconds

```rivr
// Declare sources
source rf_rx   @lmp  = rf;            // LoRa receive stream (Lamport clock)
source kbd     @mono = usb;           // USB/UART input (mono clock)
source tick           = timer(30000); // fires every 30 s

// Build pipelines with |>
let filtered = rf_rx
  |> filter.kind("CHAT")
  |> map.lower()
  |> window.ticks(5);

// Emit results to sinks
emit {
  io.lora.tx(filtered);       // retransmit over LoRa
  io.usb.print(kbd);          // echo keyboard input to serial
  io.lora.beacon(tick);       // send a BEACON packet every 30 s
}
```

**Built-in pipe operators:** `filter.pkt_type` · `filter.kind` · `filter.nonempty` · `map.upper` · `map.lower` ·
`map.trim` · `window.ticks` · `window.ms` · `throttle.ticks` · `delay.ticks` · `budget.toa_us` ·
`fold.count` · `fold.sum` · `fold.last` · `merge`

Full grammar: [docs/en/language-reference.md](docs/en/language-reference.md)

---

## Hardware table

| Board | Radio | PA | Environments |
|---|---|---|---|
| ESP32 DevKit + E22-900M30S | SX1262 | +30 dBm external | `client_esp32devkit_e22_900` · `repeater_esp32devkit_e22_900` |
| LilyGo LoRa32 v2.1 | SX1276 | +20 dBm onboard | `client_lilygo_lora32_v21` · `repeater_lilygo_lora32_v21` |

Full pin-wiring tables and antenna notes: [docs/en/build-guide.md](docs/en/build-guide.md)

---

## Key features

| Feature | Detail |
|---|---|
| **Reactive DSL** | Composable `filter / window / merge / budget / emit` pipeline |
| **Zero heap after boot** | All engine state in BSS; deterministic memory footprint |
| **EU868 duty-cycle limiter** | 1-hour sliding window, 512-slot ring buffer with LRU eviction; 10 % default (g3) |
| **Mesh routing** | Flood (TTL/hop/dedupe) + unicast (reverse-path cache) + 16-slot pending queue |
| **Rivr Fabric** | Congestion-aware relay suppression for repeater nodes |
| **Application services** | CHAT · TELEMETRY · MAILBOX · ALERT — structured service dispatch with `@CHT`/`@TEL`/`@MAIL`/`@ALERT` JSON log records; 8-entry LRU mailbox store in BSS |
| **OTA program push** | `PKT_PROG_PUSH` delivers a new RIVR program over the mesh; Ed25519-signed, hot-reloaded from NVS |
| **Policy engine** | `@PARAMS` over the mesh updates beacon interval, TX power, relay throttle, and node role at runtime; `@POLICY` JSON response reports current state and metrics |
| **Signed `@PARAMS`** | Optional HMAC-SHA-256 PSK authentication of `@PARAMS` updates; `sig=<64hex>` wire field; rejected frames emit `@PARAMS REJECTED sig` |
| **USB origination gate** | `rivr_policy_allow_origination()` blocks new chat originations when airtime budget is depleted; rejected frames emit `@DROP` |
| **OLED UI** | SSD1306 128×64; 7 auto-rotating pages: overview, RF, routing, duty-cycle, RIVR VM, neighbours, Fabric |
| **34 metric counters** | Emitted as `@MET` JSON; attach `@SUPPORTPACK` to every bug report |
| **Simulation mode** | 8-round mesh simulation without SX1262 hardware (`RIVR_SIM_MODE`) |

---

## What's shipped (post-v0.1)

- ✅ **HAL abstraction** — `hal/radio_if.h`, `hal/crypto_if.h`, `hal/feature_flags.h` unified compile-time knob table
- ✅ **Rivr Fabric** — congestion-aware relay suppression for repeater nodes (60 s sliding-window score)
- ✅ **Signed OTA programs** — `PKT_PROG_PUSH` payload verified with Ed25519 before NVS write; anti-replay sequence counter
- ✅ **Policy engine** — `@PARAMS` runtime parameter updates; role-based relay throttle; USB origination gate; `@POLICY` JSON metrics; `policy` CLI command
- ✅ **Signed `@PARAMS`** — optional HMAC-SHA-256 PSK authentication; `sig=<64hex>` wire field; metrics: `sig_ok` / `sig_fail`
- ✅ **OLED display** — SSD1306 128×64 seven-page rotating UI; FreeRTOS task on CPU1
- ✅ **Application services** — `PKT_TELEMETRY` (8), `PKT_MAILBOX` (9), `PKT_ALERT` (10) with dedicated C-layer handlers (`rivr_svc.c`); structured `@TEL`/`@MAIL`/`@ALERT` JSON log records; 8-entry LRU static mailbox store

## Roadmap

- **next** — Frequency hopping / channel rotation; multi-channel duty-cycle tracking
- **future** — Trace system (`tag()` operator → `@TRACE` JSON log line); per-source metrics frame

---

## Documentation

| Topic | English | Nederlands |
|---|---|---|
| Overview | [en/overview.md](docs/en/overview.md) | [nl/overzicht.md](docs/nl/overzicht.md) |
| Architecture | [en/architecture.md](docs/en/architecture.md) | [nl/architectuur.md](docs/nl/architectuur.md) |
| Language reference | [en/language-reference.md](docs/en/language-reference.md) | [nl/taalreferentie.md](docs/nl/taalreferentie.md) |
| Build guide | [en/build-guide.md](docs/en/build-guide.md) | [nl/bouwhandleiding.md](docs/nl/bouwhandleiding.md) |
| Firmware integration | [en/firmware-integration.md](docs/en/firmware-integration.md) | [nl/firmware-integratie.md](docs/nl/firmware-integratie.md) |
| Wire protocol | [en/protocol.md](docs/en/protocol.md) | — |
| Application services | [en/services.md](docs/en/services.md) | — |
| Release process | [releasing.md](docs/releasing.md) | — |

---

## Example programs

Ready-to-use `.rivr` programs in [`examples/`](examples/):

| File | Description |
|---|---|
| [chat_relay.rivr](examples/chat_relay.rivr) | Full mesh relay — CHAT, DATA, TELEMETRY, ALERT, MAILBOX with per-service EU868 duty-cycle budgets |
| [telemetry_periodic.rivr](examples/telemetry_periodic.rivr) | Telemetry aggregation gateway — relay, 30 s reception window, frame count, heartbeat |
| [store_forward_mailbox.rivr](examples/store_forward_mailbox.rivr) | PKT_MAILBOX store-and-forward with windowed buffer and delayed re-flood; backward-compatible PKT_CHAT track |

Load a program over the mesh with the OTA push mechanism:

```bash
# Sign and push (requires a signing key matching rivr_pubkey.h)
rivr_sign --key key0.pem --key-id 0 --seq 1 examples/chat_relay.rivr > payload.bin
# (Delivery via PKT_PROG_PUSH is handled by the mesh transport layer)
```

---

## Project layout

```
Rivr/
├── .github/workflows/ci.yml    — CI: Rust (fmt+clippy+test) + C tests
├── CMakeLists.txt              — ESP-IDF top-level project
├── platformio.ini              — PlatformIO build environments
├── Cargo.toml                  — Rust workspace (rivr_core + rivr_host)
│
├── firmware_core/              — C hardware drivers and firmware logic
│   ├── main.c                  — app_main(), tx_drain_loop(), sim rounds
│   ├── radio_sx1262.c/.h       — SX1262 driver (E22)
│   ├── radio_sx1276.c/.h       — SX1276 driver (LilyGo)
│   ├── routing.c/.h            — flood + jitter + forward-budget caps
│   ├── route_cache.c/.h        — unicast reverse-path cache
│   ├── pending_queue.c/.h      — 16-slot pending queue
│   ├── protocol.c/.h           — wire format + CRC-16
│   ├── dutycycle.c/.h          — EU868 sliding-window limiter (512-slot, LRU)
│   ├── rivr_metrics.c/.h       — 34-counter metrics + @SUPPORTPACK
│   ├── rivr_fabric.c/.h        — Rivr Fabric congestion relay policy
│   ├── rivr_policy.c/.h        — runtime @PARAMS policy, role enforcement, origination gate, HMAC sig
│   ├── rivr_ota.c/.h           — signed PKT_PROG_PUSH gate (Ed25519 + anti-replay)
│   ├── crypto/hmac_sha256.c/.h — self-contained SHA-256 + HMAC-SHA-256 (no heap)
│   └── display/display.c/.h    — SSD1306 7-page UI (FreeRTOS task, CPU1)
│
├── rivr_core/src/              — Rust library (no_std + alloc)
│   ├── parser.rs               — recursive-descent parser
│   ├── compiler.rs             — AST → engine DAG
│   ├── ffi.rs                  — #[no_mangle] C-ABI exports + freeze + safety contract
│   └── runtime/                — Engine, Scheduler, Value, Stamp
│
├── rivr_host/src/              — Host tooling (rivrc compiler CLI, replay)
├── rivr_layer/                 — RIVR ↔ C firmware glue (init, sinks, sources)
│   ├── rivr_svc.c/.h           — Application service handlers (CHAT/TELEMETRY/MAILBOX/ALERT)
│   └── rivr_programs/          — Built-in RIVR program strings
├── variants/                   — Board-specific config headers
├── examples/                   — Ready-to-use .rivr programs
├── tools/                      — Host utilities (rivr_decode wire decoder)
└── tests/                      — C test suites (acceptance, recovery, replay, dutycycle, OTA)
```

---

## License

Source code: **MIT** (see `LICENSE`).

> **Radio regulatory notice:** LoRa transmissions are subject to national radio regulations.
> The default 869.480 MHz / +30 dBm configuration targets the EU868 g3 sub-band (duty-cycle
> 10 %, ERP ≤ 1 W).  You are responsible for compliance with the regulations in your
> jurisdiction before operating any radio transmitter.

