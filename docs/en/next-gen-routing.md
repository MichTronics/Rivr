# Next-Generation Routing — Developer Reference

This document is the authoritative reference for the multi-phase routing
upgrade shipped in Rivr firmware versions supporting
`RIVR_FEATURE_AIRTIME_ROUTING`, `RIVR_FEATURE_ADAPTIVE_FLOOD`, and
`RIVR_FEATURE_OPPORTUNISTIC_FWD`.

---

## Table of Contents

1. [Overview](#overview)
2. [Feature Flags](#feature-flags)
3. [Phase Summary](#phase-summary)
4. [Memory Impact](#memory-impact)
5. [Tuning Knobs](#tuning-knobs)
6. [CLI Commands](#cli-commands)
7. [Telemetry & Monitoring](#telemetry--monitoring)
8. [Phase Interaction Diagram](#phase-interaction-diagram)
9. [Migration Notes](#migration-notes)
10. [Known Edge Cases](#known-edge-cases)

---

## Overview

The next-gen routing stack is a series of backward-compatible, feature-flag
gated improvements to the Rivr flood-relay pipeline.  Each phase builds on
the previous one and can be enabled independently per build variant.

**Core principle:** reduce redundant relay traffic without ever risking
connectivity for edge nodes or sparse networks.  Every suppression decision
has a conservative fallback that defaults to relay when in doubt.

---

## Feature Flags

All three flags live in `firmware_core/hal/feature_flags.h` and default to
**role-based values** — active for REPEATER/GATEWAY builds, off for CLIENT
and generic builds.

| Flag | Default (REPEATER/GW) | Default (CLIENT) | Source file |
|--|--|--|--|
| `RIVR_FEATURE_AIRTIME_ROUTING` | `1` | `0` | `feature_flags.h` |
| `RIVR_FEATURE_ADAPTIVE_FLOOD` | `1` | `0` | `feature_flags.h` |
| `RIVR_FEATURE_OPPORTUNISTIC_FWD` | `1` | `0` | `feature_flags.h` |

To override for a specific variant, add a `#define` **before** including
any firmware header in a variant `config.h`, or pass `-DRIVR_FEATURE_xxx=1`
at compile time.

---

## Phase Summary

| Phase | Commit | Feature flags | What it does |
|--|--|--|--|
| **0** | `2b70a56` | None (always on) | Telemetry foundation: `rivr_metrics_t` counters, `g_rivr_metrics` singleton, `rtstats` CLI command, `@MET` / `@RST` log-parseable JSON blocks |
| **1** | `0dfa130` | `AIRTIME_ROUTING` | ETX×8 fixed-point metric added to `rivr_neighbor_t` (`etx_x8`). `neighbor_link_score_full()` computes ETX-weighted score when flag=1; identical to `neighbor_link_score()` when flag=0 |
| **2** | `9145417` | `AIRTIME_ROUTING` | Route cache scoring and next-hop selection use ETX×8. `route_cache_best_hop()` selects best next-hop by composite score (RSSI × ETX penalty). Counters: `airtime_route_selected_total`, `airtime_route_fallback_total`. CLI: `ntable` |
| **3** | `0a25c70` | `ADAPTIVE_FLOOD` | `routing_fwdbudget_adapt()` throttles `forward_budget_t.max_fwd_count` based on combined channel load (max of fabric score and duty-cycle %), three tiers: <40% no-throttle, 40–69% half-cap, ≥70% quarter-cap, floor=4 fwd/min |
| **4** | `7840eba` | `OPPORTUNISTIC_FWD` | `opfwd_suppress.h`: reactive relay suppression. When a DEDUPE-drop frame has `PKT_FLAG_RELAY` set, the overheard (src\_id, pkt\_id) is recorded in `g_opfwd_suppress`. TX drain checks before transmit; cancels relay if neighbour already forwarded it. Counter: `flood_fwd_cancelled_opport_total` |
| **5** | `63671ec` | `OPPORTUNISTIC_FWD` | `fwd_set.h`: proactive relay suppression and tiered hold-off. Builds a ranked list of relay candidates from the live neighbour table. Suppresses relay when viable neighbours exist but this node's link is too weak. Adds hold-off tiers to space out relays by quality. Counter: `flood_fwd_score_suppressed_total`. CLI: `fwdset` |

---

## Memory Impact

| Component | Location | Size | Notes |
|--|--|--|--|
| `g_rivr_metrics` | BSS | ~164 bytes | All counters; always allocated |
| `rivr_neighbor_t` × 16 | BSS (`g_ntable`) | 384 bytes | Phase 1+ fields (etx\_x8, avg\_frame\_len) included always |
| `opfwd_suppress_t` | BSS (`g_opfwd_suppress`) | 196 bytes | Phase 4; zero-init in BSS, no explicit init needed |
| `fwd_set_t` | Stack only | 20 bytes | Phase 5; never in BSS, built on-demand in relay hot-path |
| `forward_budget_t` | BSS | 20 bytes | Phase 3; adapt called once/minute |
| Route cache entries × 32 | BSS | 640 bytes | Unchanged since v1 |
| Pending queue | BSS | ~2 kB | Unchanged since v1 |

**Total new static memory from Phases 1–5:** ~620 bytes in REPEATER/GW builds.

---

## Tuning Knobs

All constants are `#define` macros in their respective headers.  Override by
defining before including the header, or by patching `config.h` for a
specific variant.

### AIRTIME_ROUTING (Phase 1–2)

| Constant | Default | File | Description |
|--|--|--|--|
| `NTABLE_SIZE` | 16 | `neighbor_table.h` | Maximum concurrent tracked neighbours |
| `NTABLE_EXPIRY_MS` | 120 000 ms | `neighbor_table.h` | Hard expiry for a neighbour entry (2 min) |
| `NTABLE_STALE_MS` | 30 000 ms | `neighbor_table.h` | Age at which a neighbour is marked STALE (30 s) |
| `NTABLE_SCORE_UNICAST_MIN` | 20 | `neighbor_table.h` | Minimum score to use a neighbour for unicast next-hop |

### ADAPTIVE_FLOOD (Phase 3)

| Constant | Default | File | Description |
|--|--|--|--|
| `FWDBUDGET_MAX_FWD_ROLE` | 30 (REPEATER), 10 (CLIENT) | `routing.h` | Max relays per window at full channel capacity |
| `FWDBUDGET_ADAPT_PERIOD_MS` | 60 000 ms | `routing.h` | Adapt window length (1 min) |
| Load tier thresholds | 40 / 70 | `routing.c` | Channel load % boundaries for half-cap / quarter-cap |

### OPPORTUNISTIC_FWD Phase 4 (reactive)

| Constant | Default | File | Description |
|--|--|--|--|
| `OPFWD_SUPPRESS_SIZE` | 16 | `opfwd_suppress.h` | Maximum (src\_id, pkt\_id) pairs tracked simultaneously |
| `OPFWD_SUPPRESS_EXPIRY_MS` | 300 ms | `opfwd_suppress.h` | Window after overhear during which relay is cancelled |

### OPPORTUNISTIC_FWD Phase 5 (proactive, fwd_set)

| Constant | Default | File | Description |
|--|--|--|--|
| `FWDSET_MAX` | 3 | `fwd_set.h` | Maximum candidates ranked in forward-candidate set |
| `FWDSET_MIN_RELAY_SCORE` | 20 | `fwd_set.h` | Minimum score for a neighbour to be "viable" (relay-capable) |
| `FWDSET_HOLDOFF_MID_MS` | 50 ms | `fwd_set.h` | Extra delay for mid-quality direct link (score 40–69) |
| `FWDSET_HOLDOFF_LOW_MS` | 120 ms | `fwd_set.h` | Extra delay for low-quality direct link (score 20–39) |

**Interaction:** `FWDSET_HOLDOFF_LOW_MS` (120 ms) is intentionally less than
`OPFWD_SUPPRESS_EXPIRY_MS` (300 ms).  This means a low-tier relay that is
not cancelled within the 120 ms extra window will still be cancelled up to
300 ms after a neighbour's overhear fires the Phase 4 reactive path.

---

## CLI Commands

All commands are available at the Rivr serial console.

| Command | Phase | Description |
|--|--|--|
| `metrics` | 0 | Emit `@MET` JSON block: all `g_rivr_metrics` counters |
| `rtstats` | 0 | Emit `@RST` JSON block: routing pipeline telemetry snapshot |
| `ntable` | 2 | Print live neighbour table with RSSI / SNR / ETX×8 / loss |
| `fwdset` | 5 | Show current forward-candidate set, suppress decision, extra hold-off, and `score_suppressed_total` counter |

Example `fwdset` output on a repeater with a moderate link:

```
Forward set: viable=2 best_direct=50 holdoff=50ms cands=[0xBBBB0002:71 0xAAAA0001:50]
  suppress=no  extra_holdoff=50 ms
  score_suppressed_total=12
```

---

## Telemetry & Monitoring

### @MET JSON keys (emitted by `metrics` command)

New keys added by next-gen routing:

| Key | Phase | Counter field | Description |
|--|--|--|--|
| `fwd_att` | 0 | `flood_fwd_attempted_total` | Relay-eligible packets (RIVR\_FWD\_FORWARD) |
| `fwd_opc` | 4 | `flood_fwd_cancelled_opport_total` | Relays cancelled by Phase 4 reactive overhear |
| `fwd_scs` | 5 | `flood_fwd_score_suppressed_total` | Relays suppressed by Phase 5 quality gate |
| `at_sel` | 2 | `airtime_route_selected_total` | Unicast next-hops via ETX scoring |
| `at_fb` | 2 | `airtime_route_fallback_total` | ETX scoring insufficient → fell back to hop-count |

### @RST JSON (emitted by `rtstats` command)

The `p0` section of every `@RST` block contains:

```json
"p0": {
  "at_sel": <airtime_route_selected>,
  "at_fb":  <airtime_route_fallback>,
  "opc":    <opport_cancelled>,
  "scs":    <score_suppressed>
}
```

### Relay suppression efficiency

To determine what fraction of relay-eligible packets were suppressed across
both Phase 4 and Phase 5:

```
suppressed_total = fwd_opc + fwd_scs
relay_reduction% = suppressed_total / fwd_att × 100
```

A healthy repeater in a dense network should show `relay_reduction%` between
20–60%.  Values above 80% may indicate the node is isolated or scoring is
miscalibrated (check `ntable` output).

---

## Phase Interaction Diagram

```
Inbound RF frame
       │
       ▼
routing_flood_forward()
       │
       ├─ RIVR_FWD_DROP_*  →  drop counters, discard
       │
       └─ RIVR_FWD_FORWARD ──────────────────────────────────────►
                                                                  │
                                                     flood_fwd_attempted++
                                                                  │
                                                     [policy gate, client filter]
                                                                  │
                                                     routing_forward_delay_ms()
                                                     → delay_ms = jitter (0..200 ms)
                                                                  │
                                                     ┌─────────────────────────┐
                                                     │  RIVR_FABRIC_REPEATER   │
                                                     │  fabric relay gate      │  (Phase 3
                                                     │  backpressure gate      │   ADAPTIVE_FLOOD
                                                     └─────────────────────────┘   active here)
                                                                  │
                                                   ┌──────────────────────────────┐
                                                   │  RIVR_FEATURE_OPPORTUNISTIC  │
                                                   │  FWD (Phase 5 proactive)     │
                                                   │                              │
                                                   │  fwdset_build(&g_ntable)     │
                                                   │        │                     │
                                                   │        ├─ suppress? → skip   │ flood_fwd_score_
                                                   │        │             enqueue │ suppressed++
                                                   │        │                     │
                                                   │        └─ holdoff tier       │
                                                   │             delay_ms += 0    │
                                                   │                    /50/120   │
                                                   └──────────────────────────────┘
                                                                  │
                                                     encode + push to rf_tx_queue
                                                     (fwd_req.due_ms = now+delay_ms)
                                                                  │
                                                             [jitter wait]
                                                                  │
                                                   ┌──────────────────────────────┐
                                                   │  RIVR_FEATURE_OPPORTUNISTIC  │
                                                   │  FWD (Phase 4 reactive)      │
                                                   │                              │
                                                   │  tx_drain: opfwd_suppress_   │
                                                   │  check() before RF TX        │
                                                   │        │                     │
                                                   │        ├─ suppress? → cancel │ flood_fwd_
                                                   │        │                     │ cancelled_opport++
                                                   │        └─ clear → TX         │
                                                   └──────────────────────────────┘
                                                                  │
                                                              RF transmit
```

---

## Migration Notes

### Upgrading from pre-Phase-0 firmware

- All 3 feature flags default to **0** for CLIENT/generic builds — no
  behaviour change.
- REPEATER/GATEWAY builds automatically enable all 3 flags.  Test relay
  behaviour in a lab mesh (≥ 3 nodes) before deploying at scale.

### RIVR_FEATURE_AIRTIME_ROUTING = 1 (Phase 2)

- Route cache scoring changes.  Existing routes learned with non-ETX
  metrics are purged on reboot (cache is volatile).
- `neighbor_link_score_full()` now returns lower scores for lossy links
  that previously "looked" good based on RSSI alone.  This may cause more
  flood-based fallbacks for links with high loss\_rate in the short term
  until neighbour stats stabilise.

### RIVR_FEATURE_ADAPTIVE_FLOOD = 1 (Phase 3)

- Forward budget is **throttled** on heavy-traffic channels.  This is
  intentional.  If a repeater in a dense network starts silently dropping
  relays, check `flood_drop_budget` in `@MET` and the `rtstats` output.
- Tune `FWDBUDGET_MAX_FWD_ROLE` down for high-density deployments.

### RIVR_FEATURE_OPPORTUNISTIC_FWD = 1 (Phase 4 + 5)

- Phase 4: ensure the TX drain loop calls `opfwd_suppress_check()` **before**
  passing a relay frame to the radio.  Missing this call makes Phase 4 a
  no-op (relay still fires, counter never increments).
- Phase 5: `fwdset_build()` is called in the relay hot-path (RF RX ISR
  context on some builds).  Keep tuning constants default unless profiling
  shows timing issues.  The function is O(NTABLE\_SIZE × FWDSET\_MAX) = O(48)
  — typically < 5 µs on ESP32.

---

## Known Edge Cases

### Edge nodes always relay (viable\_count = 0 safety)

`fwdset_suppress_relay()` returns `false` when `viable_count == 0` regardless
of `best_direct_score`.  This ensures a node with **no** viable neighbours
(score ≥ 20) never self-silences.  Without this safety, an edge node
at the fringe of the mesh would suppress every relay, partitioning the
network it was supposed to extend.

**Implication:** `fwd_scs` counter will never increment on a truly isolated
node.  Non-zero `fwd_scs` + `viable_count ≥ 1` in `fwdset` output confirms
Phase 5 is active and suppressing weak relays.

### Phase 4 + Phase 5 interaction on low-tier relays

When `best_direct_score` is in the 20–39 range (low tier), Phase 5 adds
+120 ms to `delay_ms`.  This is less than `OPFWD_SUPPRESS_EXPIRY_MS`
(300 ms), so:

1. The best-positioned relay fires within its normal jitter window
   (0–200 ms).
2. That best relay is overheard → Phase 4 records the suppression entry
   for 300 ms.
3. The low-tier relay's 120 ms penalty fires at most 320 ms after the
   original packet RX.
4. If the best relay fired within the 300 ms window, Phase 4 cancels the
   low-tier copy.

Net result: low-tier relays are cancelled ~70–80% of the time in a typical
3-node cluster, reducing redundant airtime without ever dropping packets in
singleton-relay topologies.

### score\_suppressed\_total growth on a well-positioned node

A repeater at the centre of a cluster — with excellent direct links to many
peers — will have a **low** `fwd_scs` and a **low** `fwd_opc` (it fires
quickly inside the jitter window, before any Phase 4 cancellation occurs).  A
weak repeater at the edge will see high `fwd_scs`.  This asymmetry is correct
and expected.

### NTABLE\_STALE\_MS vs NTABLE\_EXPIRY\_MS

`fwdset_build()` skips entries where `(now - last_seen_ms) > NTABLE_STALE_MS`
(30 s).  `neighbor_link_score_full()` checks `NTABLE_EXPIRY_MS` (120 s) and
returns 0 for older entries.  The stale check in `fwdset_build` is more
conservative: a neighbour silent for 30–120 s is not counted as "viable"
even though its entry is still in the table.  This prevents a ghost entry
from blocking suppression.

### Clock wraparound

All age comparisons use unsigned 32-bit millisecond arithmetic.
`tb_millis()` wraps at ~49.7 days.  The comparisons are wrap-safe provided
the difference between any two timestamps is < 2^31 ms (~24.8 days), which
exceeds all Rivr timeout constants by orders of magnitude.
