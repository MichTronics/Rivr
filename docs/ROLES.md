# RIVR Node Roles

RIVR firmware supports three compile-time roles. Each role is selected via a build flag
(`-DRIVR_ROLE_xxx=1`) and its corresponding PlatformIO environment. A node can hold only one
role per flash; the mutual-exclusion guard in `feature_flags.h` emits a `#error` if more than
one is enabled.

If no role flag is set (simulation / unit-test builds) the firmware compiles in **generic / sim**
mode with mid-range defaults; no role-specific init function runs.

---

## Role Overview

| Role | Build flag | PlatformIO env | Relay CHAT/DATA? | Fabric | CLI |
|---|---|---|---|---|---|
| **Client** | `RIVR_ROLE_CLIENT=1` | `client_<board>` | ❌ | ❌ | ✅ UART0 |
| **Client + BLE** | `RIVR_ROLE_CLIENT=1` + `RIVR_FEATURE_BLE=1` | `client_<board>_ble` | ❌ | ❌ | ✅ UART0 + BLE |
| **Repeater** | `RIVR_ROLE_REPEATER=1` | `repeater_<board>` | ✅ flood + fabric | ✅ | ❌ |
| **Gateway** | `RIVR_ROLE_GATEWAY=1` | custom (see below) | ✅ all types | optional | USB bridge |

---

## Compile-Time Capacity Defaults

All sizes are set in `firmware_core/hal/feature_flags.h` and guarded by `#ifndef` so they can
be overridden per-variant via a variant `config.h` or a `-D` build flag.

| Constant | CLIENT | REPEATER / GATEWAY | Generic (sim/test) |
|---|---|---|---|
| `RIVR_ROUTE_CACHE_SIZE` → `RCACHE_SIZE` | 32 | 64 | 64 |
| `RIVR_RETRY_TABLE_SIZE` → `RETRY_TABLE_SIZE` | 8 | 32 | 16 |
| `FWDBUDGET_MAX_FWD_ROLE` | 20 fwd/type/min | 60 fwd/type/min | 30 (base budget) |

To override for a specific variant, add to its `config.h`:

```c
#define RIVR_ROUTE_CACHE_SIZE  128u   // larger cache for dense deployment
#define RIVR_RETRY_TABLE_SIZE   48u   // more in-flight ACK slots
```

---

## 1. Client

A **Client** node is an end-user device. It sends and receives `PKT_CHAT` / `PKT_DATA` locally
but **does not relay** user traffic. This prevents channel saturation by devices that have no
infrastructure role.

Control frames (`PKT_BEACON`, `PKT_ROUTE_REQ/RPL`, `PKT_ACK`, `PKT_PROG_PUSH`) are still
forwarded so the mesh routing layer remains intact.

### Boot log (example)

```
I [rivr] role: CLIENT | relay_budget=20 fwd/type/min | rc_cap=32 | retry_cap=8 | CLI enabled
```

### Serial CLI (UART0, 115 200 baud)

```
> status                         — full node status (see below)
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

#### `rivr status` output

```
=== RIVR node status ===
  node_id       : 0xdeadbeef
  callsign      : NODE1
  net_id        : 0x0001
  uptime_ms     : 123456789
  role          : CLIENT
  rx_frames     : 1024
  tx_frames     : 512
  neighbors     : 3
  routes        : 7 / 32  (active / capacity)
  pending       : 2
  retry_cap     : 8
  relay_budget  : 20 fwd/type/min
  rc_hit        : 612
  rc_miss       : 89
Routing metrics:
  rreq_rx       : 14
  rreq_target   : 3
  rreq_cache    : 11
  rreq_suppress : 0
  rrpl_rx       : 14
  rrpl_learn    : 14
  fwd_ttl_drop  : 0
  pq_drained    : 12
  pq_expired    : 2
Retry / reliability:
  ack_rx        : 102
  ack_tx        : 98
  retry_attempt : 4
  retry_ok      : 3
  retry_fail    : 1
  retry_fallback: 1
  flood_fallback: 0
Loop guard:
  loop_drops    : 0
========================
```

### Build & flash

```bash
# Without BLE
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900     -t upload
~/.platformio/penv/bin/pio device monitor -e client_esp32devkit_e22_900

# With BLE bridge (_ble variant available for every supported board)
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900_ble -t upload
~/.platformio/penv/bin/pio device monitor -e client_esp32devkit_e22_900_ble
```

---

## 2. Repeater

A **Repeater** is a mains-powered (or large-battery) infrastructure node. It receives all
packet types and re-transmits eligible ones after:

1. **RIVR Fabric congestion gate** — 60-second sliding-window score per network segment;
   `PKT_CHAT` and `PKT_DATA` can be `DELAY`-ed or `DROP`-ped when congested.
2. **Airtime token-bucket** — global 10 % duty budget + per-neighbour 2 % sub-budget.
3. **EU868 duty-cycle hard cap** — 1 % / hour; enforced by `dutycycle.c`.

Control frames always bypass suppression and are relayed immediately.

### Boot log (example)

```
I [rivr] role: REPEATER | relay_budget=60 | rc_cap=64 | retry_cap=32 | fabric=on
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

### Key metrics

| `@MET` key | Meaning |
|---|---|
| `fab_drop` | Frames dropped by Fabric congestion gate |
| `fab_delay` | Frames deferred by Fabric gate |
| `cls_chat` | Frames dropped by airtime token gate (chat class) |
| `dc_blk` | Frames blocked by EU868 duty-cycle hard cap |
| `ble_conn` | Cumulative BLE client connections (0 when `RIVR_FEATURE_BLE=0`) |
| `ble_rx` | Frames received from BLE client and injected into mesh |
| `ble_tx` | Frames forwarded to BLE client via TX notify |
| `ble_err` | BLE stack errors (mbuf alloc fail, dropped writes) |

### Build & flash

```bash
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900 -t upload
~/.platformio/penv/bin/pio device monitor -e repeater_esp32devkit_e22_900
```

---

## 3. Gateway *(stub — IP bridge not yet implemented)*

A **Gateway** bridges the RIVR mesh to an IP network (MQTT broker, database, REST API, etc.).
It relays all packet types like a Repeater and additionally pipes every received frame over a
USB/serial transport to a host process.

The `RIVR_ROLE_GATEWAY` flag is wired into the role-capacity system (`feature_flags.h`) and
the `rivr_init_gateway()` boot function, but the IP bridge itself is a stub — see the
`TODO(gateway): rivr_gateway_bridge_init()` comment in `firmware_core/main.c`.

### Boot log (example)

```
I [rivr] role: GATEWAY | relay_budget=60 | rc_cap=64 | retry_cap=32 | IP bridge: stub
```

### Planned platformio.ini environment

```ini
[env:gateway_esp32devkit_e22_900]
extends = env:repeater_esp32devkit_e22_900
build_flags =
    ${env:repeater_esp32devkit_e22_900.build_flags}
    -URIVR_ROLE_REPEATER
    -DRIVR_ROLE_GATEWAY=1
```

When the bridge is implemented, pipe the serial output into `rivr_host`:

```bash
rivr_host --port /dev/ttyUSB0 --baud 115200 --mqtt mqtt://broker.local
```

See [docs/en/firmware-integration.md](en/firmware-integration.md) for the sink/source API used
to wire the bridge.

---

## Role Selection Quick Reference

```
Does the node need to relay traffic?
    ├─ Yes, RF only — infrastructure node  →  Repeater
    ├─ Yes, bridge to IP network           →  Gateway   (stub)
    └─ No — end-user device               →  Client
```

---

## Compile-Time Flag Reference

| Flag | Default | Effect |
|---|---|---|
| `RIVR_ROLE_CLIENT` | 0 | End-device mode: relay suppressed, CLI enabled, small buffers |
| `RIVR_ROLE_REPEATER` | 0 | Infrastructure relay: Fabric + congestion gate, large buffers |
| `RIVR_ROLE_GATEWAY` | 0 | Like Repeater + IP bridge stub; large buffers |
| `RIVR_ROUTE_CACHE_SIZE` | role-derived | `RCACHE_SIZE` override; see capacity table above |
| `RIVR_RETRY_TABLE_SIZE` | role-derived | `RETRY_TABLE_SIZE` override; see capacity table above |
| `RIVR_FABRIC_REPEATER` | 0 | Enable congestion-aware relay suppression (auto on for Repeater/Gateway) |
| `RIVR_FEATURE_BLE` | 0 | Enable BLE transport bridge (Nordic NUS service, BOOT\_WINDOW/BUTTON/APP\_REQUESTED activation modes); requires `sdkconfig.ble` + `CONFIG_BT_ENABLED=y` |
| `RIVR_SIM_MODE` | 0 | Software simulation — no SX1262 hardware |
| `RIVR_RF_FREQ_HZ` | 869480000 | RF centre frequency in Hz |
| `FEATURE_DISPLAY` | 1 | Enable SSD1306 I²C display task |
| `RIVR_FAULT_INJECT` | 0 | Enable fault-injection hooks (CI only) |
