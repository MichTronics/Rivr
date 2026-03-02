# RIVR Node Roles

RIVR firmware supports four distinct node roles. Each role is selected at **compile-time** via
build flags and PlatformIO environments. A node can only hold one role per flash.

---

## Role Overview

| Role | Build flag | PlatformIO env | Relay chat/data? | Fabric | CLI | Display |
|---|---|---|---|---|---|---|
| **Repeater** | `RIVR_BUILD_REPEATER=1` | `repeater_esp32devkit_e22_900` | ✅ flood + fabric | ✅ On | ❌ | ✅ (OLED page 7) |
| **Client** | `RIVR_ROLE_CLIENT=1` | `client_esp32devkit_e22_900` | ❌ | ❌ | ✅ UART0 chat | ✅ |
| **Gateway** | *(manual, see below)* | custom | ✅ all types | optional | USB bridge | optional |
| **Monitor** | *(manual, see below)* | custom | ❌ — RX only | ❌ | USB dump | optional |

---

## 1. Repeater

A **Repeater** is a mains-powered (or large-battery) node responsible for extending mesh coverage.
It receives all packet types and re-transmits eligible ones after passing them through:

1. **RIVR Fabric congestion gate** — 60-second sliding-window score per network segment;
   `PKT_CHAT` and `PKT_DATA` can be `DELAY`-ed or `DROP`-ped when the network is congested.
2. **Airtime token-bucket** — global 10 % duty budget + per-neighbour 2 % sub-budget.
3. **EU868 duty-cycle hard cap** — 1 % / hour; enforced by `dutycycle.c`.

Control frames (`PKT_BEACON`, `PKT_ROUTE_REQ/RPL`, `PKT_ACK`, `PKT_PROG_PUSH`) always bypass
all suppression and are relayed immediately.

### Build & flash

```bash
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900 -t upload
~/.platformio/penv/bin/pio device monitor -e repeater_esp32devkit_e22_900
```

### OLED display pages

| # | Content |
|---|---|
| 1 | System overview: uptime, node ID, role |
| 2 | RF stats: RSSI, SNR, rx/tx frame counts |
| 3 | Routing: route cache size, dedupe drops |
| 4 | Duty cycle: airtime used (µs), blocks |
| 5 | RIVR VM status |
| 6 | Neighbours: callsigns, RSSI/SNR, score |
| 7 | **Fabric debug**: congestion score, relay rate, DELAY/DROP counts |

Pages auto-rotate every 5 seconds.

### Key metrics to watch

| `@MET` key | Meaning |
|---|---|
| `fab_drop` | Frames dropped by Fabric congestion gate |
| `fab_delay` | Frames deferred by Fabric gate |
| `cls_chat` | Frames dropped by airtime token gate (chat class) |
| `dc_blk` | Frames blocked by EU868 duty-cycle hard cap |

---

## 2. Client

A **Client** node is an end-user device. It sends and receives `PKT_CHAT` / `PKT_DATA` locally
but **does not relay** them. This prevents channel saturation by devices that have no
infrastructure role.

Control frames are still forwarded (BEACON, ROUTE_REQ/RPL, ACK, PROG_PUSH) so the mesh
routing layer stays intact.

### Serial CLI (UART0, 115200 baud)

```
> chat hello from node B        — broadcast PKT_CHAT
> id                             — print node ID, callsign and net ID
> info                           — build info (env, sha, radio profile)
> metrics                        — print @MET JSON (all counters)
> policy                         — print @POLICY JSON (params + counters)
> supportpack                    — @SUPPORTPACK JSON snapshot
> neighbors                      — live neighbour table (RSSI/SNR/score)
> routes                         — route cache (score, age)
> set callsign CALL              — set and persist callsign
> set netid 0x0001               — set and persist network ID
> log debug                      — set log verbosity (debug|metrics|silent)
> help                           — show this list
```

Received `PKT_CHAT` frames are printed automatically:

```
[CHAT][deadbeef]: hello from node B
>
```

The `> ` prompt is reprinted after each incoming message so mid-type input is not lost.

### Build & flash

```bash
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900 -t upload
~/.platformio/penv/bin/pio device monitor -e client_esp32devkit_e22_900
```

---

## 3. Gateway

A **Gateway** bridges the RIVR mesh to an IP network (MQTT broker, database, REST API, etc.).
It relays all packet types like a Repeater, but in addition pipes every received frame over a
USB/serial transport to a host process (e.g. `rivr_host` Rust tooling or a custom Python bridge).

### Recommended configuration

```ini
# platformio.ini custom env
[env:gateway_esp32devkit_e22_900]
extends = env:repeater_esp32devkit_e22_900
build_flags =
    ${env:repeater_esp32devkit_e22_900.build_flags}
    -DRIVR_GATEWAY_MODE=1
```

With `RIVR_GATEWAY_MODE=1` the `rivr_sinks.c` `usb_print` sink is enabled for every
received frame. Pipe the serial output into `rivr_host`:

```bash
rivr_host --port /dev/ttyUSB0 --baud 115200 --mqtt mqtt://broker.local
```

> **Note:** Gateway mode is not implemented in the default firmware build; it requires
> wiring `rivr_sources.c` and `rivr_sinks.c` for your target transport. See
> [docs/firmware-integration.md](en/firmware-integration.md) for the sink/source API.

---

## 4. Monitor / Sniffer

A **Monitor** node receives every packet on the air and prints decoded frames to USB serial
without retransmitting anything. Useful for debugging, site surveys, and traffic analysis.

### Recommended configuration

```ini
[env:monitor_esp32devkit_e22_900]
extends = env:repeater_esp32devkit_e22_900
build_flags =
    ...
    -DRIVR_MONITOR_ONLY=1   ; disable all TX paths
    -DRIVR_FABRIC_REPEATER=0
```

With `RIVR_MONITOR_ONLY=1` the `tx_drain_loop()` is compiled as a no-op and `rivr_sinks.c`
directs all received frames to the `usb_print` sink with full header dump.

Pair with the `rivr_host` replay tool to replay a live capture:

```bash
# Capture to JSONL
rivr_host --port /dev/ttyUSB0 --capture trace.jsonl

# Replay later
rivr_host --replay trace.jsonl
```

---

## Role Selection Quick Reference

```
Do nodes need to relay traffic?
    ├─ Yes, with congestion awareness  →  Repeater
    ├─ Yes, bridge to IP               →  Gateway
    └─ No

        Send/receive user messages?
            ├─ Yes                     →  Client
            └─ No, pure analysis       →  Monitor
```

---

## Compile-Time Flag Reference

| Flag | Default | Effect |
|---|---|---|
| `RIVR_BUILD_REPEATER` | 0 | Enable Fabric + headless repeater path |
| `RIVR_FABRIC_REPEATER` | 0 | Enable congestion-aware relay suppression |
| `RIVR_ROLE_CLIENT` | 0 | Disable relay of CHAT/DATA; enable CLI |
| `RIVR_GATEWAY_MODE` | 0 | Enable USB bridge for every received frame |
| `RIVR_MONITOR_ONLY` | 0 | Disable all TX; USB-print all RX |
| `RIVR_SIM_MODE` | 0 | Software simulation — no SX1262 hardware |
| `RIVR_RF_FREQ_HZ` | 869480000 | RF centre frequency in Hz |
| `FEATURE_DISPLAY` | 1 | Enable SSD1306 I²C display task |
| `RIVR_FAULT_INJECT` | 0 | Enable fault-injection hooks (CI only) |
