# Rivr — Device Roles

Rivr firmware supports three mutually exclusive **compile-time roles**.
Exactly one role flag must be set per build; the compiler enforces this with
a static `#error` if two roles are combined, and a `#pragma message` warning
if none is set (valid only for test/sim builds).

---

## Role overview

| Role                  | Flag                  | Typical hardware           | Purpose                          |
|-----------------------|-----------------------|----------------------------|----------------------------------|
| **CLIENT**            | `RIVR_ROLE_CLIENT=1`  | ESP32 DevKit + E22 (client env) | End-user node — originates chat/data, limited relay |
| **REPEATER**          | `RIVR_ROLE_REPEATER=1`| ESP32 DevKit + E22 (repeater env) | Infrastructure node — relay hub |
| **GATEWAY** *(stub)*  | `RIVR_ROLE_GATEWAY=1` | Any ESP32 board            | Future IP-bridge — stub only     |

---

## RIVR_ROLE_CLIENT

### Purpose
The **client** is the end-user facing node.  It sends and receives application
packets (chat, data), initiates route discovery, and drains its pending queue
when a route is resolved.

### Enabled subsystems
| Subsystem | Enabled? | Notes |
|---|---|---|
| Serial CLI (`rivr_cli`) | ✅ | Full command set including `status`, `chat`, `routes`, `metrics` |
| RF relay (flood forward) | ✅ | Control frames only (`ROUTE_REQ`, `ROUTE_RPL`, `BEACON`) |
| Chat/data relay | ❌ | Clients do not re-broadcast `PKT_CHAT` or `PKT_DATA` |
| Congestion fabric (`RIVR_FABRIC_REPEATER`) | ❌ | Fabric scoring is a repeater-only feature |
| Route cache | ✅ | Used to hold resolved routes |
| Pending queue | ✅ | Queues outgoing frames until route is resolved |

### Forward budget
```
FWDBUDGET_MAX_FWD_ROLE = 20  (forwards / type / minute)
```
Conservative limit — clients are not relay hubs and should not flood the
channel at high rates.

### Build flag
Set in `platformio.ini` `build_flags` or via `-D`:
```
-DRIVR_ROLE_CLIENT=1
```

---

## RIVR_ROLE_REPEATER

### Purpose
The **repeater** is an infrastructure node optimised for relay throughput.
It relays all packet classes (`PKT_CHAT`, `PKT_DATA`, control frames) and
participates fully in the hybrid unicast control plane.

### Enabled subsystems
| Subsystem | Enabled? | Notes |
|---|---|---|
| Serial CLI (`rivr_cli`) | ❌ | No interactive CLI in repeater builds |
| RF relay (flood forward) | ✅ | All pkt_types forwarded subject to budget |
| Chat/data relay | ✅ | Relays `PKT_CHAT` and `PKT_DATA` to all neighbours |
| Congestion fabric (`RIVR_FABRIC_REPEATER`) | Optional | Enable with `-DRIVR_FABRIC_REPEATER=1` |
| Route cache | ✅ | Replies to `ROUTE_REQ` on behalf of known destinations |
| Pending queue | ✅ | |

### Forward budget
```
FWDBUDGET_MAX_FWD_ROLE = 60  (forwards / type / minute)
```
Three times the client budget — allows the repeater to serve multiple
simultaneous paths without dropping legitimate relay traffic.

### Route-reply eligibility
Repeaters reply to `ROUTE_REQ` for any destination in their cache that passes
all four eligibility gates (see `route_cache_can_reply_for_dst()` in
`route_cache.h`):
- Valid + non-expired
- Not `RCACHE_FLAG_PENDING`
- `metric >= RCACHE_REPLY_MIN_METRIC` (30)
- `hop_count <= RCACHE_REPLY_MAX_HOPS` (3)

### Build flag
```
-DRIVR_ROLE_REPEATER=1
```
Pair with `-DRIVR_FABRIC_REPEATER=1` for congestion-aware relay suppression
on high-density deployments.

---

## RIVR_ROLE_GATEWAY *(stub — future)*

### Purpose
The **gateway** is reserved for a future IP-bridge implementation.  When set:
- The node boots and initialises the radio.
- No active packet relay is performed by the firmware.
- The application layer is expected to bridge RF ↔ IP externally.

### Current state
This role is a **compile-time stub**.  Setting `RIVR_ROLE_GATEWAY=1` causes:
1. The build banner to show `role=gateway`.
2. Main-loop boot log to print `role: GATEWAY | stub — no active relay`.
3. The "no role" pragma warning to be suppressed.

No gateway-specific MAC, IP stack, or bridging code is included yet.

### Build flag
```
-DRIVR_ROLE_GATEWAY=1
```

---

## Compile-time guards

`firmware_core/hal/feature_flags.h` enforces role exclusivity:

```c
// Error: two roles set at once
#if RIVR_ROLE_CLIENT && RIVR_ROLE_REPEATER
#  error "RIVR: Both RIVR_ROLE_CLIENT and RIVR_ROLE_REPEATER are set — pick one."
#endif

#if RIVR_ROLE_GATEWAY && (RIVR_ROLE_CLIENT || RIVR_ROLE_REPEATER)
#  error "RIVR: RIVR_ROLE_GATEWAY cannot be combined with RIVR_ROLE_CLIENT or RIVR_ROLE_REPEATER."
#endif

// Warning: no role (valid for tests/sim only)
#if !RIVR_ROLE_CLIENT && !RIVR_ROLE_REPEATER && !RIVR_ROLE_GATEWAY && !RIVR_SIM_MODE
#  pragma message("RIVR: No role flag set ... Using generic defaults.")
#endif
```

---

## Role vs. variant vs. board

| Concept | File | Sets |
|---|---|---|
| **Role** | `platformio.ini` `build_flags` | `RIVR_ROLE_*`, `RIVR_FABRIC_REPEATER` |
| **Variant** | `variants/<board>/config.h` | Pin map, radio chip selection, RF frequency |
| **Board** | PlatformIO `board` field | Flash, SRAM, CPU target |

A variant config does **not** set the role.  The same board variant can be
flashed as either CLIENT or REPEATER simply by choosing the appropriate
PlatformIO environment.

---

## CLI `status` command (CLIENT only)

On client nodes, `status` prints a concise one-screen snapshot:

```
Status:
  role          : client
  node_id       : 0x1A2B3C4D
  callsign      : CALL1
  net_id        : 0x0001
  uptime_ms     : 123456
  rx_frames     : 42
  tx_frames     : 17
  neighbors     : 3
  routes        : 5
  pending_queue : 0
  loop_drops    : 0
  rc_hit        : 31
  rc_miss       : 6
```

The same information is available in the `@MET` JSON stream for automated
monitoring.
