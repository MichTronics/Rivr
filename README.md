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
  |> filter.kind("CHAT")
  |> budget.airtime(3600, 0.10, 280000)   -- EU868 g3 duty-cycle guard
  |> throttle.ticks(1);                   -- max once per Lamport tick

emit { io.lora.tx(chat); }               -- rebroadcast over LoRa
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

**Built-in pipe operators:** `filter.kind` · `filter.nonempty` · `map.upper` · `map.lower` ·
`map.trim` · `window.ticks` · `throttle.ticks` · `delay.ticks` · `budget.airtime` ·
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
| **OTA program push** | `PKT_PROG_PUSH` delivers a new RIVR program over the mesh; hot-reloaded from NVS |
| **OLED UI** | SSD1306 128×64; 7 auto-rotating pages: overview, RF, routing, duty-cycle, RIVR VM, neighbours, Fabric |
| **34 metric counters** | Emitted as `@MET` JSON; attach `@SUPPORTPACK` to every bug report |
| **Simulation mode** | 8-round mesh simulation without SX1262 hardware (`RIVR_SIM_MODE`) |

---

## Roadmap

- **v0.2** — Trace system (`tag()` operator → `@TRACE` JSON log line); per-source metrics frame
- **v0.3** — Signed `PKT_PROG_PUSH` (ED25519 manifest); policy engine (`permit` / `deny` rules)
- **v0.4** — Frequency hopping / channel rotation; multi-channel duty-cycle tracking

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
| Release process | [releasing.md](docs/releasing.md) | — |

---

## Example programs

Ready-to-use `.rivr` programs in [`examples/`](examples/):

| File | Description |
|---|---|
| [chat_relay.rivr](examples/chat_relay.rivr) | Full mesh relay with EU868 duty-cycle guard and beacon |
| [telemetry_periodic.rivr](examples/telemetry_periodic.rivr) | Periodic heartbeat / uptime counter transmitted over LoRa |
| [store_forward_mailbox.rivr](examples/store_forward_mailbox.rivr) | Buffered store-and-forward with capped window and delayed re-flood |

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

