# Rivr Mesh Routing

This document explains how Rivr routes packets through a LoRa mesh network.

For the detailed technical specification including the next-generation ETX
scoring and opportunistic forward suppression, see
[docs/en/next-gen-routing.md](en/next-gen-routing.md).

---

## Overview

Rivr uses a **hybrid routing strategy** that combines:

1. **Flood routing** — simple, resilient, no topology knowledge needed
2. **Unicast routing** — targeted delivery using a reverse-path cache
3. **Neighbor-quality scoring** — ETX-weighted next-hop selection

The firmware makes a per-frame routing decision on every received packet and
selects the best available strategy for that packet type.

---

## Packet flow

```
RF receive
    │
    ▼
Duplicate check  (src_id, pkt_id) dedupe ring ──→ DROP if seen
    │
    ▼
TTL check  TTL == 0? ──→ DROP
    │
    ▼
Routing decision
    ├─ Unicast? → route_cache_best_hop() → next-hop SX1262 TX
    │              └─ miss → pending_queue → flood fallback
    └─ Flood?  → routing_next_hop_score() → relay or suppress
                 ├─ RIVR_FWD_FORWARD  → jitter delay → SX1262 TX
                 ├─ RIVR_FWD_DELAY    → Fabric backpressure applied
                 └─ RIVR_FWD_DROP     → TTL/policy/loop gate dropped
```

---

## Flood routing

Flood routing is the default for multicast packet types (CHAT, BEACON,
TELEMETRY, ALERT).

### How it works

1. When a packet is received the node checks its **dedupe ring** — a 64-entry
   circular buffer of `(src_id, pkt_id)` pairs.  If the pair is already
   present the packet is silently dropped.
2. If TTL > 0: decrement TTL, add `(src_id, pkt_id)` to the dedupe ring,
   apply a **jitter delay** (0–255 ms), re-transmit.
3. The jitter delay prevents all nodes hearing a frame from re-transmitting
   simultaneously (the "flood storm" problem).

### Rivr Fabric relay suppression

On repeater nodes (`RIVR_FABRIC_REPEATER=1`), the **Rivr Fabric** layer adds
congestion-aware relay scoring.  Each candidate relay is scored by a 60-second
sliding-window algorithm.  Frames with a score above the drop threshold are
dropped; frames above the delay threshold get an additional hold-off.

This allows a dense mesh to self-throttle without coordinator intervention.

---

## Unicast routing

Unicast routing uses a **reverse-path cache** populated by ROUTE_RPL frames.

### Route discovery

1. Node A wants to send a unicast frame to node C but has no route.
2. Node A enqueues the frame in the **pending queue** (up to 16 slots).
3. Node A floods a `PKT_ROUTE_REQ` to the mesh.
4. Any node that *is* node C or *has a route to* node C replies with
   `PKT_ROUTE_RPL`.
5. Node A receives the reply, writes the next-hop into the **route cache**,
   and drains the pending queue.

### Route cache

- 16 slots, BSS-allocated (no heap)
- Each entry stores: `dst_id`, `next_hop`, `hop_count`, `score`, `timestamp`
- Entries expire after `RIVR_ROUTE_TIMEOUT_MS` (default 5 minutes)
- The score is a composite of RSSI, SNR, hop count, age decay, and loss rate

### Neighbor-quality next-hop selection

`route_cache_best_hop()` uses a three-tier decision:

1. **Best scored cache entry** — highest composite score from the route cache
2. **Direct-peer fallback** — `neighbor_best()` returns the best direct
   neighbor if the cache has no valid entry
3. **Flood fallback** — if no direct neighbor qualifies, fall back to flood

---

## Neighbor table

The **neighbor table** tracks up to 16 peers (`RIVR_MAX_NEIGHBORS`) observed
on the radio channel.

Each entry stores:
- **EWMA RSSI** and **EWMA SNR** — exponentially-weighted moving averages
- **Seq-gap loss rate** — fraction of missing sequence numbers per peer
- **ETX estimate** — Expected Transmission Count based on loss rate
- **Flags** — `DIRECT` (recent beacon heard), `STALE` (>2 min silent),
  `BEACON` (only beacon frames observed)
- **Last-seen timestamp** — for expiry tracking

Entries are updated on every received frame via `neighbor_update()` and
expire after `RIVR_NEIGHBOR_TIMEOUT_MS` (default 2 minutes).

---

## Pending queue and retry

When a unicast destination is unreachable:

1. The frame is held in the **pending queue** (16 slots) while route discovery
   runs.
2. If an ACK is not received within `RIVR_RETRY_TIMEOUT_MS`, the **retry
   table** attempts up to N retransmissions.
3. After retry exhaustion, a **flood fallback** is issued as a last resort.

---

## Duty-cycle gating

All outbound frames (relay and originated) pass through the **duty-cycle
limiter** before transmission.  The limiter enforces the EU868 g3 10% budget
using a 1-hour sliding window with 512 slots and LRU eviction.

If the budget is depleted, the frame is dropped and `duty_blocked` is
incremented.  The CLI `metrics` command shows the current duty-cycle usage.

---

## Beacon strategy

PKT_BEACON frames allow a node to announce its presence to neighbors that have
not yet received any unicast traffic from it.  In dense EU868 environments
(many nodes on the same channel) uncoordinated beaconing can saturate the
channel even before any application traffic is sent.  The beacon scheduler
(`firmware_core/beacon_sched.c`) reduces this risk through four mechanisms:

### 1. Long default interval

| Role | Default interval | Default jitter |
|---|---|---|
| Client (unset) | 600 000 ms (10 min) | ± 120 000 ms (2 min) |
| Repeater / Gateway | 300 000 ms (5 min) | ± 60 000 ms (1 min) |

A hard minimum of **60 000 ms (1 min)** is enforced in the policy layer;
values below this are rejected and counted in `beacon_config_rejected_total`.

### 2. RIVR timer as poll tick, not TX trigger

The RIVR program sets `source beacon_tick = timer(60000)` — a 60-second *poll
tick*.  The C layer (`beacon_sink_cb`) then decides whether to actually
transmit based on elapsed time and neighbor state.  This means tightening
the timer value at the RIVR layer **does not** increase beacon rate.

### 3. Startup burst

At boot the node immediately sends one PKT_BEACON so neighbors are learned
quickly without waiting up to 10 minutes.  Up to `BEACON_STARTUP_COUNT` (2)
further startup beacons are sent when the timer fires within the first
`BEACON_STARTUP_WINDOW_MS` (300 000 ms / 5 min) of uptime — bypassing the
long interval for that brief window only.

### 4. Adaptive neighbor suppression

If `live_neighbor_count ≥ BEACON_SUPPRESS_MIN_NEIGHBORS` (2) the scheduled
beacon is silently skipped and `beacon_suppressed_total` is incremented.
A node that can already see two or more live neighbors does not need to beacon
further to make itself discoverable; keeping quiet reduces channel load in
the exact scenarios where it matters most (dense deployments).

### Airtime accounting

PKT_BEACON frames are no longer classified as `PKTCLASS_CONTROL` (always
free).  They now fall under `PKTCLASS_BEACON`, which draws from the global
airtime token bucket.  On a saturated channel a flood of beacons from
mis-configured nodes will drain their own budget before touching the budgets
of application-traffic classes.

---

## Routing metrics

Key counters from `rivr> metrics` (or `@MET` JSON lines):

| Counter | Meaning |
|---|---|
| `rx_dedupe_drop` | Duplicates suppressed |
| `rx_ttl_drop` | Frames dropped at TTL=0 |
| `flood_fwd_attempted_total` | Relay attempts before suppression |
| `flood_fwd_cancelled_opport_total` | Opportunistic suppression |
| `route_cache_hit_total` | Unicast cache hits |
| `route_cache_miss_total` | Cache misses → route discovery |
| `route_rpl_learn_total` | New routes learned from RPL frames |
| `neighbor_route_used_total` | Next-hop selected via neighbor score |
| `duty_blocked` | TX blocked by duty-cycle gate |
| `fabric_drop` | Fabric relay suppression drops |
| `bcn_tx` | Beacon frames transmitted |
| `bcn_start` | Beacon startup-burst frames transmitted |
| `bcn_supp` | Beacon TX suppressed (neighbor or interval) |
| `bcn_drop` | Beacon frames dropped by airtime gate |

---

## Configuration

Routing parameters are configurable in `firmware_core/rivr_config.h` and can
be overridden per variant in `platformio.ini`:

```ini
build_flags =
    -DRIVR_ROUTE_TIMEOUT_MS=300000    ; route cache lifetime (ms)
    -DRIVR_NEIGHBOR_TIMEOUT_MS=120000 ; neighbor expiry (ms)
    -DRIVR_MAX_ROUTES=16              ; route cache slots
    -DRIVR_MAX_NEIGHBORS=16           ; neighbor table slots
    -DRIVR_MAX_PENDING=16             ; pending queue slots
```

---

## Further reading

- [docs/en/architecture.md](en/architecture.md) — full system architecture
- [docs/en/next-gen-routing.md](en/next-gen-routing.md) — ETX scoring and opportunistic suppression
- [docs/en/protocol.md](en/protocol.md) — wire packet format
