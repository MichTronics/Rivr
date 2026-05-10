# Rivr — Reliable LoRa Mesh Networking for Embedded Systems

> A lightweight networking layer and reactive dataflow runtime for LoRa radios.
> Enables resilient multi-hop communication on constrained embedded hardware.
> Runs on ESP32 + SX1262/SX1276. **Zero heap allocation after boot.**

[![CI](https://github.com/MichTronics/Rivr/actions/workflows/ci.yml/badge.svg)](https://github.com/MichTronics/Rivr/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: ESP32](https://img.shields.io/badge/platform-ESP32-green.svg)
![Status: Public Beta](https://img.shields.io/badge/status-public%20beta-orange.svg)

---

> **Public beta** — Core runtime, test suite (295 passing), and supported hardware configurations
> are stable. Wire protocol and DSL API may change between beta releases.
> Not yet recommended for unattended production deployments.
> When filing a bug, always attach a [`@SUPPORTPACK`](#metrics--diagnostics) capture.

---

## What is Rivr?

Rivr is a **LoRa mesh networking layer** designed for embedded devices.

Instead of building a single application on top of LoRa, Rivr provides a robust communication
substrate that other systems can build upon. Nodes form self-organizing radio networks without
requiring gateways or central infrastructure.

On top of that substrate, Rivr ships a **reactive dataflow DSL** that lets you describe packet
processing pipelines in a few lines of code, compiled on-device at boot and evaluated on every
incoming frame with bounded worst-case latency — no OS, no garbage collector, no heap.

```rivr
source rf_rx @lmp = rf;           // LoRa receive stream (Lamport clock)

let chat = rf_rx
  |> filter.pkt_type(1)           // PKT_CHAT only
  |> budget.toa_us(300000, 0.10, 280000)  // EU868 g3 duty-cycle guard
  |> throttle.ticks(1);           // deduplicate flood copies

let alerts = rf_rx
  |> filter.pkt_type(10)          // PKT_ALERT — elevated budget
  |> budget.toa_us(300000, 0.20, 280000);

emit { io.lora.tx(chat);   }
emit { io.lora.tx(alerts); }
```

Write the pipeline once. Rivr handles duty-cycle compliance, flood deduplication, and
congestion-aware relay suppression automatically.

---

## The story behind Rivr

Rivr didn't start as a big plan or a startup idea.
It started the way many radio projects start: on a workbench with a few LoRa modules and a lot of curiosity.

I've always been fascinated by radio communication and decentralized systems. As a radio amateur
(PD4MV) I enjoy experimenting with ways devices can communicate without relying on centralized
infrastructure.

When LoRa mesh projects like Meshtastic and MeshCore started appearing, I immediately began
experimenting with them. They proved that long-range decentralized communication was possible with
very little hardware.

But while building and testing networks with ESP32 boards and SX1262 radios, I kept running into
the same thought:

> *What if a LoRa mesh network could behave more like a living system instead of a fixed routing table?*

Real radio environments are unpredictable.
Signals fade. Nodes move. Noise appears. Links disappear.

Yet most LoRa mesh implementations treat the network like a static road system:

```
A → B → C → D
```

One path. One route.

But radio doesn't really work like that.

During many evenings of experimenting, logging packets, and debugging strange behavior on the air,
the idea slowly evolved into a different model:

> *A network where packets flow dynamically through the mesh, adapting to real radio conditions.*

More like water flowing through a river system.

Water doesn't follow a single rigid path — it spreads, adapts, and finds the best route forward.

That idea became the core philosophy behind the project.

**And that's where the name Rivr comes from.**

Rivr is my attempt to explore what a next-generation LoRa mesh architecture could look like:

- deterministic embedded behavior
- airtime-aware networking
- adaptive relay selection
- resilient packet propagation
- and a programmable runtime for packet processing

What started as small experiments with ESP32 boards has grown into a much larger exploration of
embedded networking, routing algorithms, and decentralized communication.

Rivr is still evolving, and the goal is simple:

> Build a mesh networking platform that is powerful enough for research and experimentation,
> but simple enough for anyone with a LoRa board to start using.

If you enjoy experimenting with radio, embedded systems, or distributed networks — you're very
welcome to join the journey.

*Michel van Veen — MichTronics — PD4MV*

---

## Design Principles

### Deterministic embedded behavior
Rivr avoids dynamic memory allocation during runtime and uses fixed-size data structures
throughout. Memory footprint is fully determined at compile time.

### Airtime awareness
LoRa networks are severely bandwidth-constrained. Rivr includes a sliding-window EU868
duty-cycle limiter, per-service airtime budgets, and flood-deduplication to prevent unnecessary
retransmissions.

### Defensive protocol parsing
Incoming packets are validated for length, CRC, TTL, and type before entering the routing
pipeline. The decode path is covered by a fuzz-testing harness.

### Radio robustness
The SX1262 driver detects and automatically recovers from BUSY lockups, spurious IRQ/SPI
faults, and TX timeouts — without requiring a full reboot. Prolonged RX silence is exposed
as telemetry instead of forcing a reset.

### Mesh safety
Rivr includes loop detection, TTL-based propagation limits, packet-level deduplication,
and congestion-aware relay suppression to prevent routing storms.

---

## Why Rivr?

|  | Rivr | Hand-rolled C | MQTT bridge |
|---|---|---|---|
| **Memory model** | Zero heap after boot | Static / heap mix | Heap-heavy |
| **Duty-cycle safety** | Built-in sliding-window limiter | Hand-rolled or absent | N/A |
| **OTA pipeline update** | `PKT_PROG_PUSH` over the mesh | Full reflash required | Topic reconfiguration |
| **Stream composition** | First-class `filter / window / merge / budget` | Ad-hoc if-else chains | Broker rules engine |
| **Target** | ESP32 bare-metal (no OS) | Any MCU | Requires IP stack |

---

## Quickstart — no hardware needed

No ESP32 required to explore Rivr. The host tools run entirely on Linux / macOS / Windows.

```bash
# Install Rust if needed
curl https://sh.rustup.rs -sSf | sh

# Build the host tools
cargo build -p rivr_host

# Compile a RIVR program and inspect the node graph
cargo run -p rivr_host --bin rivrc -- examples/chat_relay.rivr

# Replay captured mesh traffic through the engine
cargo run -p rivr_host -- tests/rivr_replay.jsonl
```

Expected output ends with: `[REPLAY] done — 42 frames processed, 0 errors`

For full toolchain setup see [docs/en/build-guide.md](docs/en/build-guide.md).

---

## Flash firmware

### ESP32 DevKit + E22-900M30S (SX1262) — primary supported board

```bash
# Client node (send + receive, no relay)
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900     -t upload

# Client + BLE bridge (append _ble to any client_* env)
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900_ble -t upload

# Dedicated repeater (Rivr Fabric congestion relay suppression enabled)
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900   -t upload
```

Monitor at 115200 baud:

```bash
~/.platformio/penv/bin/pio device monitor
```

Default air parameters: **869.480 MHz · SF8 · BW 62.5 kHz (`RF_BANDWIDTH_HZ=62500`) · CR 4/8 · preamble 16**
Override at build time: `build_flags = -DRIVR_RF_FREQ_HZ=915000000`

See [docs/FLASHING.md](docs/FLASHING.md) for all board variants and wiring tables.

---

## Hardware support

### ESP32 / ESP32-S3 (serial flash)

| Board | Radio | PA | Status |
|---|---|---|---|
| ESP32 DevKit + E22-900M30S | SX1262 | +30 dBm ext. | **Supported** |
| LilyGo LoRa32 v2.1 | SX1276 | +20 dBm onboard | **Supported** |
| Heltec WiFi LoRa 32 V2 | SX1276 | +20 dBm onboard | Experimental |
| Heltec WiFi LoRa 32 V3 | SX1262 | +22 dBm onboard | Experimental |
| Heltec WiFi LoRa 32 V4 | SX1262 | +17 dBm onboard | Experimental |
| LilyGo T-Beam v1.1 (SX1262) | SX1262 | +22 dBm onboard | Experimental ⚠ AXP192 |
| LilyGo T3-S3 | SX1262 | +22 dBm onboard | Experimental |
| Seeed XIAO ESP32S3 + Wio-SX1262 | SX1262 | +17 dBm onboard | Experimental |

### nRF52840 (DFU — adafruit-nrfutil)

| Board | Radio | PA | Status |
|---|---|---|---|
| RAK WisMesh / WisPocket (RAK4631) | SX1262 | +17 dBm onboard | **Supported** |
| Heltec T114 | SX1262 | +17 dBm onboard | Experimental |
| Seeed SenseCAP T1000-E | LR1110 | +22 dBm onboard | Experimental |

### RP2040 (UF2 / picotool)

| Board | Radio | PA | Status |
|---|---|---|---|
| Waveshare RP2040-LoRa-HF | SX1262 | +22 dBm onboard | **Supported** |

**Supported** = CI-tested and field-validated.
**Experimental** = compiles and boots; limited field testing.
Full flash commands and pin-wiring tables: [docs/FLASHING.md](docs/FLASHING.md)

---

## DSL reference

The Rivr DSL describes packet-processing pipelines that are compiled on-device at boot.
All state lives in BSS; the engine evaluates the graph on every received frame.

```rivr
// Sources
source rf_rx   @lmp  = rf;            // LoRa receive stream (Lamport clock)
source kbd     @mono = usb;           // USB/UART keyboard input
source tick           = timer(30000); // fires every 30 s

// Pipelines
let filtered = rf_rx
  |> filter.kind("CHAT")
  |> map.lower()
  |> window.ticks(5);

// Sinks
emit {
  io.lora.tx(filtered);       // retransmit over LoRa
  io.usb.print(kbd);          // echo to serial monitor
  io.lora.beacon(tick);       // periodic beacon
}
```

**Pipe operators:** `filter.pkt_type` · `filter.kind` · `filter.nonempty` · `map.upper` · `map.lower` · `map.trim` · `window.ticks` · `window.ms` · `throttle.ticks` · `delay.ticks` · `budget.toa_us` · `fold.count` · `fold.sum` · `fold.last` · `merge`

Full grammar: [docs/en/language-reference.md](docs/en/language-reference.md)

---

## Feature set

Rivr is a full networking stack, not just a radio wrapper.

| Feature | Detail |
|---|---|
| **Reactive DSL** | Composable `filter / window / merge / budget / emit` pipeline; compiled on-device at boot |
| **Zero heap after boot** | All engine state in BSS; deterministic memory footprint |
| **EU868 duty-cycle limiter** | 1-hour sliding window, 512-slot ring buffer, LRU eviction; 10 % default (g3 sub-band) |
| **Mesh routing** | Flood (TTL / hop / dedupe) + unicast (reverse-path cache) + 16-slot pending queue |
| **Neighbor-quality tracking** | EWMA RSSI/SNR + seq-gap loss-rate per peer; 16-slot BSS table; `DIRECT` / `STALE` / `BEACON` flags |
| **Three-tier next-hop selection** | Best scored cache entry → direct-peer fallback → flood |
| **Rivr Fabric** | Congestion-aware relay suppression for repeater nodes; 60 s sliding-window score |
| **Application services** | CHAT · TELEMETRY · MAILBOX · ALERT with structured `@CHT` / `@TEL` / `@MAIL` / `@ALERT` JSON log records |
| **OTA program push** | `PKT_PROG_PUSH` delivers a new RIVR program over the mesh; Ed25519-signed, anti-replay, hot-reloaded from NVS |
| **Policy engine** | `@PARAMS` updates beacon interval, TX power, relay throttle, and node role at runtime |
| **Signed `@PARAMS`** | Optional HMAC-SHA-256 PSK authentication; `sig=<64hex>` wire field |
| **OLED UI** | SSD1306 128×64; 7 auto-rotating pages: overview · RF · routing · duty-cycle · VM · neighbours · Fabric |
| **80 metric counters** | Emitted as `@MET` JSON; aggregated into `@SUPPORTPACK` for bug reports |
| **BLE transport bridge** | Optional Bluedroid-based edge interface (Nordic NUS UUIDs); same Rivr frames over BLE as LoRa; 3 activation modes; `RIVR_FEATURE_BLE=1` |
| **Simulation mode** | 8-round mesh simulation without radio hardware (`RIVR_SIM_MODE`) |

---

## Metrics & diagnostics

Every node continuously tracks 63 counters across routing, duty-cycle, OTA, and radio layers.
Run `supportpack` from the serial monitor to emit a self-contained JSON diagnostic block:

```
rivr> supportpack
@SUPPORTPACK {"ver":1,"rad_rx":142,"rad_tx":38,"rad_crc":0,"dc_blk":0,"fab_drop":0,...}
```

**Attach `@SUPPORTPACK` output to every bug report.**
Healthy baseline (10 min idle): `rad_stall=0`, `rad_crc≤2`, `dc_blk=0`, `pq_exp=0`.

---

## Example programs

| File | Description |
|---|---|
| [chat_relay.rivr](examples/chat_relay.rivr) | Full mesh relay — CHAT, DATA, TELEMETRY, ALERT, MAILBOX with per-service EU868 budgets |
| [telemetry_periodic.rivr](examples/telemetry_periodic.rivr) | Telemetry aggregation gateway — 30 s reception window, frame count, heartbeat |
| [store_forward_mailbox.rivr](examples/store_forward_mailbox.rivr) | PKT_MAILBOX store-and-forward with windowed buffer and delayed re-flood |

Push a new program over the mesh:

```bash
rivr_sign --key key0.pem --key-id 0 --seq 1 examples/chat_relay.rivr > payload.bin
# Delivery via PKT_PROG_PUSH is handled by the mesh transport layer
```

---

## Companion app

**Rivr Companion** is a Flutter app for Android, Linux, and Windows.
It connects to a node over USB serial or Bluetooth LE and provides:

- Live chat view
- Neighbour link-quality table (RSSI, SNR, score)
- Mesh topology and routing table
- Real-time `@MET` metric dashboard and `@SUPPORTPACK` export

Download the latest release from the [Companion page](https://rivr.co.nl/companion) on the Rivr website.

---

## What's in v0.1.0-beta

- ✅ **HAL abstraction** — `hal/radio_if.h`, `hal/crypto_if.h`, `hal/feature_flags.h` unified compile-time knob table
- ✅ **Rivr Fabric** — congestion-aware relay suppression for repeater nodes (60 s sliding-window score)
- ✅ **Signed OTA programs** — `PKT_PROG_PUSH` payload verified with Ed25519 before NVS write; anti-replay sequence counter
- ✅ **Policy engine** — `@PARAMS` runtime parameter updates; role-based relay throttle; USB origination gate; `@POLICY` JSON metrics
- ✅ **Signed `@PARAMS`** — optional HMAC-SHA-256 PSK authentication; `sig=<64hex>` wire field; metrics: `sig_ok` / `sig_fail`
- ✅ **OLED display** — SSD1306 128×64 seven-page rotating UI; FreeRTOS task on CPU1
- ✅ **Application services** — PKT_TELEMETRY (8), PKT_MAILBOX (9), PKT_ALERT (10); structured `@TEL` / `@MAIL` / `@ALERT` JSON log records; 8-entry LRU mailbox store
- ✅ **Neighbor table** — 16-slot BSS table; EWMA RSSI/SNR; seq-gap loss-rate; `DIRECT` / `STALE` / `BEACON` flags; auto-expiry
- ✅ **Neighbor-aware next-hop routing** — three-tier decision; composite score weights RSSI+SNR, hop count, age decay, loss rate
- ✅ **BLE transport bridge** — Bluedroid-based edge interface using Nordic NUS UUIDs (6E400001/002/003); same binary Rivr frames over BLE as over LoRa; BOOT_WINDOW (120 s) / BUTTON (5 min) / APP_REQUESTED activation modes; enabled via `RIVR_FEATURE_BLE=1` + `sdkconfig.ble`

## Roadmap

- **next** — Frequency hopping / channel rotation; multi-channel duty-cycle tracking
- **future** — Trace system (`tag()` operator → `@TRACE` JSON); per-source metrics frame

## Implementation Status

Rivr is already well beyond prototype stage, but not every roadmap item is complete yet.

| Area | Status | Notes |
|---|---|---|
| Core runtime | Done | Deterministic main loop, fixed-size structures, zero-heap-after-boot model |
| Radio driver architecture | Done | ISR-safe radio path, recovery logic, SPSC ring buffer design |
| Multi-hop forwarding | Done | TTL, dedupe, jittered relay forwarding, loop protection |
| Duty-cycle enforcement | Done | Sliding-window limiter and airtime gates are implemented |
| Reliability layer | Done | Retry table, ACK handling, retransmit/fallback flow |
| Metrics & diagnostics | Done | JSON metrics, supportpack, replay tooling, diagnostics views |
| Supported hardware | Done | ESP32 + SX1262/SX1276 support is present |
| Test suite | Done | Current docs claim 295 checks |
| Link awareness | Done | RSSI/SNR tracking, EWMA smoothing, loss tracking, ETX-style scoring |
| Path scoring | Done | Route and neighbor scoring already influence forwarding decisions |
| Companion ecosystem | Partial | Android, Linux, and Windows companion app exists and is functional |
| BLE / USB edge connectivity | Partial | Present and usable, still evolving in behavior and UX |
| OTA update flow | Partial | OTA program delivery exists, but full polished user flow is still evolving |
| Smart mesh routing | Partial | Strong foundations exist, but not the full long-term routing vision yet |
| Opportunistic forwarding | Partial | Core building blocks exist, but not fully realized yet |
| Adaptive relay election | Partial | Partly implemented through scoring, suppression, and fabric logic |
| Mesh visualizer | Partial | Network/node views exist, but not yet a mature visualizer |
| Trace event system | Partial | Observability is strong, but not yet the full trace model described in the roadmap |
| Frequency hopping | Planned | Not implemented yet |
| Adaptive channel rotation | Planned | Not implemented yet |
| Per-channel duty-cycle tracking | Planned | Not implemented yet |
| Self-organizing mesh intelligence | Planned | Future work |
| Distributed optimization / load balancing | Planned | Future work |

### Summary

- Done: ~55%
- Partial: ~25%
- Planned: ~20%

Rivr is already strong in runtime quality, radio reliability, routing foundations, and diagnostics.

The biggest remaining work is in advanced adaptive radio behavior, full opportunistic mesh intelligence, and a more polished end-user ecosystem.

---

## Documentation

| Topic | English | Nederlands |
|---|---|---|
| **Quickstart** | [docs/quickstart.md](docs/quickstart.md) | — |
| **Flashing guide** | [docs/FLASHING.md](docs/FLASHING.md) | — |
| **Build guide** | [docs/BUILDING.md](docs/BUILDING.md) · [docs/en/build-guide.md](docs/en/build-guide.md) | [docs/nl/bouwhandleiding.md](docs/nl/bouwhandleiding.md) |
| **User config** | [firmware_core/user_config_template.h](firmware_core/user_config_template.h) | — |
| Overview | [docs/en/overview.md](docs/en/overview.md) | [docs/nl/overzicht.md](docs/nl/overzicht.md) |
| Architecture | [docs/en/architecture.md](docs/en/architecture.md) | [docs/nl/architectuur.md](docs/nl/architectuur.md) |
| Language reference | [docs/en/language-reference.md](docs/en/language-reference.md) | [docs/nl/taalreferentie.md](docs/nl/taalreferentie.md) |
| Firmware integration | [docs/en/firmware-integration.md](docs/en/firmware-integration.md) | [docs/nl/firmware-integratie.md](docs/nl/firmware-integratie.md) |
| Wire protocol | [docs/en/protocol.md](docs/en/protocol.md) | — |
| Application services | [docs/en/services.md](docs/en/services.md) | — |
| **Companion app BLE** | [docs/en/companion-ble-integration.md](docs/en/companion-ble-integration.md) | — |
| **RML (Message Layer)** | [docs/RML.md](docs/RML.md) | — |
| Release process | [docs/releasing.md](docs/releasing.md) | — |

---

## Project layout

```
Rivr/
├── firmware_core/          — C firmware: radio drivers, routing, duty-cycle, policy, OTA, display
├── rivr_core/src/          — Rust library (no_std + alloc): parser, compiler, runtime, FFI
├── rivr_host/src/          — Host tooling: rivrc compiler CLI, replay engine
├── rivr_layer/             — RIVR ↔ C firmware glue: init, sinks, sources, service handlers
├── examples/               — Ready-to-use .rivr programs
├── tests/                  — C test suites: acceptance, recovery, replay, dutycycle, OTA, policy
├── tools/                  — Host utilities: rivr_decode wire decoder
├── variants/               — Board-specific config headers
├── docs/                   — Documentation (English + Nederlands)
├── platformio.ini          — PlatformIO build environments
└── Cargo.toml              — Rust workspace (rivr_core + rivr_host)
```

Pre-built Companion releases are available on the [Rivr website](https://rivr.co.nl/companion).

---

## Security considerations

Before deploying nodes outside a development environment:

- **Set a custom PSK** — the default `RIVR_PARAMS_PSK_HEX` is all-zeros and intentionally fails
  a compile-time `_Static_assert` when `RIVR_FEATURE_SIGNED_PARAMS=1`.
  Generate a key: `openssl rand -hex 32`
- **Enable signed OTA** — `RIVR_FEATURE_SIGNED_OTA=1` requires Ed25519 signature on every
  `PKT_PROG_PUSH` frame. Unsigned pushes are silently discarded.
- **Respect duty-cycle limits** — the default 869.480 MHz / +30 dBm target is the EU868 g3
  sub-band (≤ 10 %, ERP ≤ 1 W). Override frequency and power for your region.
- **Default settings are for development only** — never deploy with the all-zero PSK or without
  reviewing `variants/<board>/config.h`.

---

## Contributing

See [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) and [.github/CODE_OF_CONDUCT.md](.github/CODE_OF_CONDUCT.md).
Before opening a PR, run:

```bash
make -C tests          # all C suites must pass (total 295 checks, 0 failures)
cargo test -p rivr_core --features std
cargo clippy -p rivr_core --features std -- -D warnings
```

Feedback, bug reports, and field-testing results are highly appreciated.

---

## License

Source code: **MIT** — see [LICENSE](LICENSE).

> **Radio regulatory notice:** LoRa transmissions are subject to national radio regulations.
> The default 869.480 MHz / +30 dBm configuration targets the EU868 g3 sub-band
> (duty-cycle ≤ 10 %, ERP ≤ 1 W). You are responsible for compliance with the regulations
> in your jurisdiction before operating any radio transmitter.

---

## Acknowledgements

Rivr builds upon ideas from the broader LoRa and embedded networking communities.
Special thanks to researchers and developers exploring decentralised radio communication systems.

---

## Vision

Rivr aims to become a reliable foundation for open radio networking — a system where embedded
devices can form resilient mesh networks without relying on centralised infrastructure,
operating predictably within radio regulations and hardware constraints.
