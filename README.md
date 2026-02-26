# RIVR — Reactive LoRa Mesh Runtime

> A minimal, deterministic stream-graph language and runtime for off-grid LoRa mesh networks,
> running on ESP32 + SX1262 with zero heap allocation after boot.

---

## What is RIVR?

RIVR is a small reactive programming language whose programs describe *data-flow pipelines* over
radio packets.  A running node continuously receives LoRa frames, passes them through the RIVR
engine (filter / transform / route), and emits results to sinks (retransmit, USB serial, log).

Key design constraints:

- **No heap after init** — all state lives in BSS; deterministic memory footprint.
- **`no_std + alloc`** Rust core — the language engine compiles to a static library that links
  into ESP-IDF C firmware without an OS allocator.
- **Bounded execution** — every tick drains at most *N* RX frames and runs at most *M* engine
  steps; worst-case latency is always bounded.
- **EU868 duty-cycle aware** — a sliding-window budget tracker enforces the 1 % per-hour
  LoRa airtime limit before every transmission.

---

## Architecture

```
┌───────────────────────────────────────────────────────────────────────┐
│  ESP32 (Xtensa LX6)                                                   │
│                                                                       │
│  ┌──────────────────────────────────────────────────────────────┐     │
│  │  firmware_core  (C)                                          │     │
│  │                                                              │     │
│  │  SX1262 ISR ──► rf_rx_ringbuf ──► rivr_sources.c             │     │
│  │                                        │                     │     │
│  │                              ┌─────────▼──────────┐          │     │
│  │                              │   RIVR Engine      │          │     │
│  │                              │  (rivr_core / Rust)│          │     │
│  │                              │  parse → compile   │          │     │
│  │                              │  → schedule → emit │          │     │
│  │                              └─────────┬──────────┘          │     │
│  │                                        │                     │     │
│  │  rf_tx_queue ◄── rivr_sinks.c ◄────────┘                     │     │
│  │       │                                                      │     │
│  │  tx_drain_loop()  (jitter gate + duty-cycle gate)            │     │
│  │       │                                                      │     │
│  │  radio_transmit() ──► SX1262                                 │     │
│  └──────────────────────────────────────────────────────────────┘     │
└───────────────────────────────────────────────────────────────────────┘

Mesh routing layer
  flood forward     → routing_flood_forward()   (TTL / hop / dedupe)
  unicast path      → route_cache_tx_decide()   (reverse-path learning)
  pending queue     → pending_queue.*            (cache-miss hold + drain on ROUTE_RPL)
  fallback          → PKT_FLAG_FALLBACK flood    (TTL=3, on TX queue full)
```

---

## Features

| Feature | Detail |
|---|---|
| RIVR language | Reactive stream-graph DSL; `filter`, `budget`, `throttle`, `window`, `merge`, `emit` |
| Packet types | CHAT · BEACON · DATA · ROUTE_REQ · ROUTE_RPL · ACK · PROG_PUSH |
| Flood routing | TTL + hop decrement, deduplication ring (src_id, seq) |
| Unicast routing | Reverse-path route cache, ROUTE_REQ / ROUTE_RPL discovery |
| Pending queue | 16-slot hold for unicast frames while waiting for a route reply |
| Forward jitter | `routing_forward_delay_ms()` — xorshift32 per (src, seq, type); up to 200 ms |
| Duty-cycle cap | Per-minute sliding window + per-hour cap (36 s/h ≈ 1 % EU868) |
| Drop statistics | Per-packet-type forward-drop counters in `forward_budget_t` |
| Unicast failover | TX queue full → fallback flood with `PKT_FLAG_FALLBACK`, TTL = 3 |
| Neighbour table | RSSI/SNR + callsign per peer; displayed on OLED page 6 |
| OLED display | SSD1306 128×64; 6 pages: node info, mesh stats, duty cycle, routing, RX quality, neighbours |
| Deferred-ISR RX | `radio_isr()` sets a flag only; all SPI work done in `radio_service_rx()` from main loop |
| OTA program push | `PKT_PROG_PUSH` delivers a new RIVR program over the mesh; stored in NVS, hot-reloaded |
| Sim mode | Full 8-round mesh simulation without SX1262 hardware (`RIVR_SIM_MODE`) |

---

## Quick Start

### 1 — Build the Rust static library

```powershell
cd rivr_core
cargo build --features ffi          # debug (development)
cargo build --features ffi --release  # release (flash to device)
```

The `main/CMakeLists.txt` finds `librivr_core.a` automatically — no manual copy needed.

### 2 — Simulation (no SX1262 hardware required)

```powershell
# PlatformIO
pio run -e esp32_sim -t upload && pio device monitor

# ESP-IDF
idf.py -DRIVR_SIM_MODE=1 build flash monitor
```

Expected output shows **8 simulation rounds**:

| Round | What it tests |
|---|---|
| R1 | 3× CHAT from NODE_A — GATE1 (TTL/hop), GATE3 (route learn) |
| R2 | 2× CHAT from NODE_C via NODE_B — route learn via relay |
| R3 | PKT_DATA — RIVR filter drop, C-layer relay |
| R4 | ROUTE_REQ for MY_NODE → ROUTE_RPL queued |
| R5 | Duplicate of R1[0] — GATE2 basic dedupe drop |
| R6 | Same (src, seq) via different relay — GATE2 key-is-(src,seq) |
| R7 | Pre-loaded pending msg for NODE_D + injected ROUTE_RPL → queue drain |
| R8 | TX queue full + unicast to NODE_A → `PKT_FLAG_FALLBACK` flood |

### 3 — Real hardware (E22-900M30S)

```bash
~/.platformio/penv/bin/pio run -e esp32_hw -t upload
~/.platformio/penv/bin/pio device monitor --baud 115200
```

Default air parameters: **869.480 MHz · SF8 · BW 125 kHz · CR 4/8 · preamble 8** → ~30 dBm via the E22 HP PA.

See [docs/en/build-guide.md](docs/en/build-guide.md) for full cross-compilation, pin wiring, and E22-900M30S TCXO/PA setup.

### 4 — Repeater build (E22-900M30S/M33S + Rivr Fabric)

A dedicated repeater variant is provided for headless relay nodes.  It enables
[RIVR Fabric](firmware_core/rivr_fabric.h) congestion-aware suppression for
relayed `PKT_CHAT` and `PKT_DATA` frames (`RIVR_FABRIC_REPEATER=1`).  All other
packet types (`PKT_ACK`, `PKT_BEACON`, `ROUTE_REQ/RPL`, `PROG_PUSH`) always
pass through unaffected.

```bash
# Build & flash
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900 -t upload
~/.platformio/penv/bin/pio device monitor -e repeater_esp32devkit_e22_900
```

**Default pins** (ESP32 DevKit + E22 wiring; change in
`variants/esp32devkit_e22_900_repeater/config.h` or via `-D` build flag):

| Signal | GPIO |
|---|---|
| SCK | 18 |
| MOSI | 23 |
| MISO | 19 |
| NSS/CS | 5 |
| BUSY | 32 |
| RESET | 25 |
| DIO1 | 33 |
| RXEN | 14 |
| TXEN | 13 |

**Default frequency:** 869.480 MHz (EU868).  Override at build time:

```bash
# AU915 example
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900 \
  --build-option='build_flags=-DRIVR_RF_FREQ_HZ=915000000'
```

Or add `-DRIVR_RF_FREQ_HZ=...` as the first line of the `build_flags` in
`platformio.ini` before the `-include` line — the `#ifndef` guard in the
variant header will leave it untouched.

---

## Project Layout

```
Rivr/
├── CMakeLists.txt          — ESP-IDF top-level project
├── platformio.ini          — PlatformIO build environments
├── sdkconfig.defaults      — ESP-IDF config overrides
├── Cargo.toml              — Rust workspace (rivr_core + rivr_host)
│
├── firmware_core/          — C hardware drivers
│   ├── main.c              — app_main(), tx_drain_loop(), sim rounds
│   ├── radio_sx1262.c/.h   — SX1262 driver; ISR sets flag only, radio_service_rx() does SPI
│   ├── routing.c/.h        — flood forward, jitter, budget caps, neighbour callsigns
│   ├── route_cache.c/.h    — unicast reverse-path cache
│   ├── pending_queue.c/.h  — 16-slot pending queue (cache-miss hold)
│   ├── protocol.c/.h       — RIVR binary wire format, CRC-16
│   ├── dutycycle.c/.h      — EU868 airtime tracker
│   ├── timebase.c/.h       — monotonic + Lamport clocks
│   ├── platform_esp32.c/.h — GPIO / SPI / LED init (RXEN + TXEN antenna switch)
│   ├── ringbuf.h           — lock-free SPSC ring buffer
│   └── display/            — SSD1306 OLED driver (6-page UI)
│
├── rivr_layer/             — RIVR ↔ C firmware glue
│   ├── rivr_embed.c/.h     — engine init, rivr_tick(), global state
│   ├── rivr_sinks.c/.h     — rf_tx / USB-print / log sinks
│   ├── rivr_sources.c/.h   — rf_rx / CLI / timer sources
│   └── rivr_programs/
│       └── default_program.h — compiled-in RIVR program
│
├── rivr_core/              — Rust library (no_std + alloc)
│   └── src/
│       ├── lib.rs          — crate root, feature flags
│       ├── ast.rs          — AST types
│       ├── parser.rs       — recursive-descent parser
│       ├── compiler.rs     — AST → engine DAG
│       ├── ffi.rs          — #[no_mangle] C-ABI exports
│       ├── runtime/        — Engine, Scheduler, Value, Stamp
│       └── adapt/          — ESP32 ringbuffer source adapters
│
├── rivr_host/              — Host tooling (Rust CLI, JSONL replay)
│   └── src/main.rs
│
└── docs/                   — Documentation (English + Dutch)
    ├── en/
    └── nl/
```

---

## RIVR Language — Quick Look

```rivr
source rf_rx @lmp = rf;
source beacon_tick = timer(30000);

let chat = rf_rx
  |> filter.pkt_type(1)                        -- pass only PKT_CHAT
  |> budget.toa_us(280000, 0.10, 280000)       -- 10 % duty-cycle guard
  |> throttle.ticks(1);                        -- at most once per Lamport tick

emit { io.lora.tx(chat); }                     -- re-broadcast over LoRa
emit { io.lora.beacon(beacon_tick); }          -- send PKT_BEACON every 30 s
```

Programs are parsed and compiled at boot time by `rivr_embed_init()`, then evaluated once per
`rivr_tick()` call in the main loop.  A new program can be pushed OTA as a `PKT_PROG_PUSH` frame
and will be hot-reloaded from NVS without a reboot.

---

## `rivr_core` Feature Flags

| Flag | Effect |
|---|---|
| `std` *(default)* | Enables `std` + `alloc` + `serde_json`; for host builds |
| `alloc` | Heap types (`Vec`, `String`) without full `std` |
| `ffi` | Emits `#[no_mangle]` C-ABI exports; link into ESP-IDF |
| `embedded` | Alias for `--no-default-features --features ffi`; pure no_std |
| *(none)* | Pure no-heap; payloads use `FixedText<64>` / `FixedBytes<64>` |

---

## Documentation

Full documentation is in [`docs/`](docs/README.md) — available in English and Dutch.

| Topic | English | Nederlands |
|---|---|---|
| Overview | [en/overview.md](docs/en/overview.md) | [nl/overzicht.md](docs/nl/overzicht.md) |
| Architecture | [en/architecture.md](docs/en/architecture.md) | [nl/architectuur.md](docs/nl/architectuur.md) |
| Language reference | [en/language-reference.md](docs/en/language-reference.md) | [nl/taalreferentie.md](docs/nl/taalreferentie.md) |
| Build guide | [en/build-guide.md](docs/en/build-guide.md) | [nl/bouwhandleiding.md](docs/nl/bouwhandleiding.md) |
| Firmware integration | [en/firmware-integration.md](docs/en/firmware-integration.md) | [nl/firmware-integratie.md](docs/nl/firmware-integratie.md) |
