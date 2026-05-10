# RML: Rivr Message Layer

Deterministic compression, priority-based relay, and loss-resilient message ordering for resource-constrained mesh networks.

---

## 1. Overview

**RML (Rivr Message Layer)** is an optional, compile-time feature layered atop RIVR's core mesh transport. It provides:

- **Status Dominance**: only the newest status update per sender+type relay forward, eliminating stale duplicates.
- **Delta Compression**: append/replace operations on message threads, with reconstruction of missing bases.
- **Repair Requests**: bounded retransmission on missing previous-message dependencies, preventing unbounded chains.
- **Relevance Decay**: priority decreases with message age, automatically demoting old messages at the relay frontier.
- **Airtime Budgeting**: per-intent duty-cycle gates and deterministic jitter, preventing hot-spot relay storms.
- **Deterministic Behavior**: no RNG, fixed memory buffers, thread-safe for use in RIVR's event loop and firmware context.

RML is **optional** and **does not affect legacy RIVR packets** when disabled. Enable via compile flag `RIVR_ENABLE_RML=1`.

---

## 2. Five Phases of Implementation

### Phase 1: Codec
- **Wire Format**: 28-byte RML header + variable payload (max 160 bytes).
- **Header Fields**: magic (0xA7), version (1), message type, flags, message ID, thread ID, prev ID, sender/target IDs, intent, priority, TTL, reliability, expiry time.
- **Payload Encoding**: FNV-1a hash-based message IDs, deterministic wire format (no malloc, fixed offsets).

### Phase 2: Policy & Caching
- **Policy Engine**: `rml_policy_decide()` evaluates relay, delay, TTL clamping, repair decisions in one call.
- **Seen Cache**: fixed ring buffer (64 entries) prevents duplicate relay.
- **Status Cache**: per-sender + state_type tracking; only newest status per (sender, type) relays forward.
- **Thread Cache**: stores recent message thread entries (sender/thread_id); tracks next_id pointers for delta reconstruction.

### Phase 3: Delta Operations & Thread Semantics
- **Delta Compression**: messages with `prev_id` marked as THREAD_APPEND or THREAD_REPLACE reference a base message.
- **Reconstruction**: policy checks thread cache for base; if missing, sets `repair_needed=true` and generates repair request.
- **Supersede**: `RML_FLAG_SUPERSEDES` in flags byte allows new thread entry to clear old entries in same thread, enabling version coalescing.
- **Thread States**: THREAD_APPEND (sequential), THREAD_REPLACE (overwrite base), THREAD_SYNC (reconstructed relay).

### Phase 4: Deterministic Airtime & Duty Budget
- **Relevance Decay**: priority -= 1 per 60 seconds of age; if age >= priority, relay suppressed.
- **Airtime Gate**: per-intent floor percentage. E.g., `INTENT_CHAT=50%` means relay only if duty headroom >= 50% of budget.
- **Status Soft Expiry**: status entries expire via soft_ttl_s after creation, cleared from cache automatically.
- **Deterministic Jitter**: FNV hash of message properties (id, sender, target, RSSI, SNR) → jitter ms, no RNG.

### Phase 5: Simulation Harness
- **N-Node Simulator**: fixed-size test-only engine simulating up to 8 nodes with queues, caches, deterministic loss.
- **Deterministic Loss**: per-packet loss via hash, not random—enables reproducible test scenarios (10%, 30%, 50% loss).
- **Naive Baseline**: relay without any RML logic (all messages forward 1:1) for performance comparison.
- **Metrics**: aggregated relay count, airtime, cache hits/misses, repair success rate per scenario.
- **8 Test Scenarios**: airtime monotonicity, chat baseline, status dominance, delta compression, missing-prev repair bounds, supersede coalescing, 3 loss levels, duty-constrained relay.

**Test Results**: 130 unit tests + 74 simulation tests, all pass. No heap allocations or RNG calls in any RML code.

---

## 3. Architecture

### Core Modules

```
firmware_core/rml/
├── rml.h           → public types, constants, policy API
├── rml.c           → init, message constructors, helpers
├── rml_codec.c     → wire encode/decode
├── rml_policy.c    → decision engine, caches, decay, airtime gates
├── rml_seen.c      → fixed ring buffer deduplication
└── rml_stats.c     → global statistics

rivr_layer/
└── rivr_sources.c  → optional RML RX dispatcher (checks RML_MAGIC, calls policy_decide)
```

### Integration Points

1. **RX Path** (`rivr_sources.c`):
   - Check first 4 bytes for RML_MAGIC (0xA7 + version).
   - If RML frame: decode, call `rml_policy_decide()`, relay or reconstruct if `decision.relay && forward_reconstructed`.
   - Otherwise: fall through to legacy RIVR decode.

2. **TX Path** (app/CLI layer):
   - Call `rml_message_init()` to set magic, version, message type, intent, priority, TTL.
   - For chat/delta/repair: call appropriate constructor (e.g., `rml_make_chat_message()`).
   - Encode with `rml_codec_encode()`, transmit as RIVR packet.

3. **Policy Context**:
   - Thread-safe: pass `rml_policy_context_t` (caches, stats) to `rml_policy_decide()`.
   - Duty tracking: context has `duty_budget_us` and `duty_remaining_us` for airtime gates.
   - Timestamps: uses `now_ms` parameter (caller provides via `esp_timer_get_time() / 1000`).

### Wire Format (28-byte header)

```
Offset  Size  Field
──────────────────────────────────────────
  0      1    magic (0xA7)
  1      1    version (1)
  2      1    type (THREAD_MSG, STATUS, DELTA, REPAIR, THREAD_SYNC)
  3      1    flags (bit 0: SUPERSEDES, bit 1–7 reserved)
  4      4    msg_id (FNV-1a hash)
  8      4    thread_id (uint32_t)
 12      4    prev_id (uint32_t, or 0 if no predecessor)
 16      4    sender_id (uint32_t)
 20      4    target_id (uint32_t, or 0 = broadcast)
 24      1    intent (CHAT, STATUS, TELEMETRY, CONTROL, etc.)
 25      1    priority (0–255, higher = more urgent)
 26      1    TTL (0–255, decrements per hop)
 27      1    reliability + state_type (packed nibbles)

[Additional fields packed into flags byte or optional trailer:]
  - expires_s (uint16_t, TTL in seconds)
  - soft_ttl_s (uint16_t, status cache expiry in seconds)
  - created_ms (uint32_t, message creation timestamp)
  - supersedes (bool, set by encoder if flag 0x01)
  - state_type (uint16_t, derived from thread_id or explicit field)

Variable: payload[0..160]
```

---

## 4. Key Features

### Status Dominance

When multiple status updates arrive from the same sender with the same `state_type`:
- Policy caches only the most recent (by `created_ms`).
- Previous entries are evicted.
- Relay priority increases for the newest; old status updates are not forwarded.

**Use Case**: temperature sensors sending periodic updates; only the latest value relays through the mesh.

### Delta Compression

Threads of messages can use delta encoding:
- **THREAD_APPEND**: new entry appends to thread (e.g., "message 1" → "message 1 + new entry 2").
- **THREAD_REPLACE**: new entry replaces the base (e.g., "overwrite base with new data").
- `prev_id` field points to the predecessor message.

If the base is missing when a delta arrives:
- Policy sets `repair_needed=true` and generates a repair request.
- On-the-fly reconstruction (cached base found) sets `forward_reconstructed=true` and relays `THREAD_SYNC`.
- Bounded retransmission: repairs only requested once per decay cycle.

**Use Case**: firmware updates (base = binary blob, delta = next chunk); chat threads (base = first message, append = replies).

### Relevance Decay

Messages older than their priority are automatically suppressed from relay:
- `age_s = (now_ms - created_ms) / 1000`
- `if age_s >= priority` → relay rejected, no TTL forward.

**Use Case**: avoid stale alerts and sensor readings lingering at the mesh frontier.

### Deterministic Jitter

No RNG. Relay delay computed via FNV hash of message properties:
```
hash = FNV1a(msg_id || thread_id || sender_id || target_id || now_ms || rssi || snr)
jitter_ms = (hash % max_jitter_ms)
delay_ms = base_delay_ms + jitter_ms
```

**Result**: identical message in identical radio condition always produces same delay (reproducible, testable).

### Airtime Budgeting

Per-intent duty-cycle floor:
- `INTENT_CHAT = 50%` floor: relay only if `duty_remaining_us / duty_budget_us >= 0.5`.
- `INTENT_STATUS = 30%` floor: stricter—status must compete harder for airtime.
- `INTENT_CONTROL = 80%` floor: control messages reserved for low-congestion windows.

**Effect**: chat doesn't starve control traffic; status doesn't monopolize the channel.

---

## 5. Compile-Time Configuration

### Enable RML
```bash
export RIVR_ENABLE_RML=1
pio run -e <environment>
```

### Disable RML (default)
```bash
# No special flag needed; RML code compiled out.
pio run -e <environment>
```

### Verify RML is Enabled in Firmware
Check log during boot:
```
[INFO] RML: enabled, magic=0xA7
```

### RML Constants (firmware_core/rml/rml.h)
```c
#define RML_SEEN_CACHE_SIZE        64      // max 64 seen entries
#define RML_STATUS_CACHE_SIZE      16      // max 16 status messages per node
#define RML_THREAD_CACHE_SIZE      16      // max 16 thread entries
#define RML_REPAIR_CACHE_SIZE      8       // max 8 pending repairs
#define RML_MAX_PAYLOAD           160      // max variable payload
#define RML_PRIORITY_DECAY_PERIOD_S 60    // seconds between priority decrements
```

---

## 6. Testing & Validation

### Unit Tests (130 tests)
```bash
make -C tests clean rml
# Output: RML tests: pass=130 fail=0
```

**Coverage**: codec (encode/decode roundtrip), policy (seen/status/thread/repair), delta reconstruction, supersede coalescing, decay, airtime gates, edge cases (empty payload, max TTL, missing prev).

### Simulation Tests (74 tests)
```bash
make -C tests clean rml  # Includes sim suite
# Output: RML sim tests: pass=74 fail=0
```

**Scenarios**:
1. **Airtime Monotonicity**: relay airtime ≤ origination airtime for each message.
2. **Chat Baseline**: chat messages relay unchanged (no coalescing expected).
3. **Status Dominance**: 3 status updates from same sender → only newest relays.
4. **Delta Compression**: append/replace chains verified for correct reconstruction.
5. **Missing-Prev Repair**: intentional packet drop → repair request bounded, base recovered.
6. **Supersede Coalescing**: version update clears old thread entries before new relay.
7. **Loss Levels**: 10% / 30% / 50% deterministic loss — policy adapts, repairs stay bounded.
8. **Duty-Constrained Relay**: with 30% duty headroom, only 30% of messages relay (airtime gate works).

### Host Test Suite
```bash
make -C tests test
```
**Result**: All tests pass, including RML, RML sim, acceptance, recovery, replay, dutycycle, policy, fabric, beacon, channel tests.

### Heap / RNG Verification
```bash
rg -n "\b(malloc|calloc|realloc|free|rand|random|esp_random)\b" firmware_core/rml tests/rml_sim.h tests/rml_sim.c tests/test_rml_sim.c tests/test_rml.c rivr_layer/rivr_sources.c
# Output: (no matches, exit code 1)
```
**Confirmed**: Zero dynamic allocation, zero RNG calls in any RML code.

### Firmware Build Validation
```bash
# RML enabled
export RIVR_ENABLE_RML=1
pio run -e client_esp32devkit_e22_900
# ✅ Success

# RML disabled (default)
unset RIVR_ENABLE_RML
pio run -e client_esp32devkit_e22_900
# ✅ Success (smaller binary)
```

---

## 7. Hardware Smoke Test Procedure

### Goal
Verify RML-enabled firmware works with legacy RIVR nodes without breakage or side effects.

### Setup
1. **Node A**: Flash with `RIVR_ENABLE_RML=1` (RML-enabled build).
2. **Node B**: Flash with default settings (legacy RIVR).
3. Both nodes on same channel, same geography (10–50 meters apart).

### Steps

#### Step 1: Boot & Logs
```
Node A (RML enabled):
  [INFO] RML: enabled, magic=0xA7
  [INFO] RML policy context initialized
  [INFO] Listening on TTY...

Node B (legacy):
  [INFO] RIVR v1 initialized
  [INFO] Listening on TTY...
```

#### Step 2: Chat Message (Node A → Node B)
- Node A CLI: `chat hello from A`
- Node B RX log: should show chat received, any RML overhead minimal.
- Verify decoding: if Node A sends RML chat (magic 0xA7), Node B's RX falls through to legacy decode and ignores (safe).

#### Step 3: Chat Reply (Node B → Node A)
- Node B CLI: `chat reply from B`
- Node A RX log: legacy RIVR packet arrives, policy processes normally.

#### Step 4: Status Message (Node A, with RML enabled)
- Node A: send status update (if CLI supports, or inject via test harness).
- Node B RX: should not error; legacy path handles gracefully or logs "unknown type".

#### Step 5: Neighbor Discovery & Metrics
- Both nodes transmit periodic beacons.
- Verify neighbor tables converge (each node sees the other).
- Check metrics on both nodes: no crashes, no dropped packets, normal duty cycle.

### Expected Outcome
- ✅ No crashes or panics.
- ✅ Legacy RIVR packets still relay and decode correctly.
- ✅ RML packets either relay unchanged (if next hop is RML-enabled) or ignored by legacy nodes.
- ✅ Duty cycle and beacon timing unaffected.
- ✅ Metrics counters increment normally.

### Troubleshooting
| Symptom | Likely Cause | Action |
|---------|--------------|--------|
| Node A crashes on boot | RML init error | Check `rml_policy_init()` call in main.c, verify memory. |
| Node B doesn't see Node A | RF path issue | Check antennas, distance, channel overlap. |
| Chat sent but not received | RX dispatch misconfiguration | Verify `rivr_sources.c` optional path is enabled only with `RIVR_ENABLE_RML=1`. |
| Neighbor table empty after 10s | Beacon not transmitting | Check `airtime_sched.c` and `dutycycle.c` gates. |

---

## 8. Debug Logging & Telemetry

### Live Policy Decisions
Add optional debug output in `rml_policy.c` when `RIVR_ENABLE_RML=1 && RML_DEBUG=1`:
```c
if (decision.relay) {
    printf("[RML] relay msg_id=0x%08x sender=%d ttl=%d delay_ms=%d\n",
           msg->msg_id, msg->sender_id, decision.next_ttl, decision.delay_ms);
}
if (decision.repair_needed) {
    printf("[RML] repair_needed prev_id=0x%08x\n", msg->prev_id);
}
```

### Policy Statistics
Access global `rml_stats` (from `rml_stats.c`):
```c
extern rml_stats_t rml_stats;

printf("[RML Stats] messages_decoded=%d, relayed=%d, suppressed=%d, repairs_sent=%d\n",
       rml_stats.messages_decoded,
       rml_stats.messages_relayed,
       rml_stats.messages_suppressed,
       rml_stats.repairs_sent);
```

### Test Harness Logging
In simulation tests, use `rml_sim_metrics_t` to inspect per-scenario results:
```c
rml_sim_metrics_t metrics = rml_sim_run_scenario(&config);
printf("Scenario: naive_relay=%d, rml_relay=%d, airtime_saved=%.1f%%\n",
       metrics.naive_relay_count,
       metrics.rml_relay_count,
       100.0 * (1.0 - (double)metrics.rml_relay_count / metrics.naive_relay_count));
```

---

## 9. RML Frame Injector (CLI / Test Command)

### Proposed Command Structure
```
rivr> rml send chat "Hello, this is RML chat"
rivr> rml send status state_type=temp priority=10 ttl=5 payload="25.3C"
rivr> rml send delta thread_id=123 prev_id=120 type=replace payload="updated"
rivr> rml send repair thread_id=123 prev_id=120 "Need base message"
```

### Implementation Sketch
1. Parse command in `rivr_cli.c` or new `rml_cli.c` submodule.
2. Call appropriate `rml_make_*_message()` constructor from `rml.c`.
3. Encode with `rml_codec_encode()`.
4. Submit to TX queue via `rivr_send()`.

### Use Cases
- **Manual Status Test**: inject 3 status updates, observe policy suppresses old ones.
- **Delta Chain Validation**: send base + 2 appends, verify reconstruction.
- **Loss Recovery**: send base, intentionally drop on-air, send repair, confirm recovery.
- **Airtime Profiling**: inject burst of chat/status/control, measure relay count vs. duty used.

---

## 10. Multi-Node Validation Matrix

### Test Topology: 2-Node Chain
```
Node A (RML) ──50m── Node B (RML)
```

**Goal**: Verify TTL, status dominance, delta reconstruction, repair bounds.

#### Test 1A: Status Dominance (2-node, sender→relay)
1. Node A sends 3 status updates (different payloads, same sender+type).
2. Node B relays (should see only the newest status, not all 3).
3. Metric: `relay_count == 1` (not 3).

#### Test 1B: Delta Chain Reconstruction
1. Node A sends base message (type=THREAD_APPEND, prev_id=0, payload="msg1").
2. Node A sends delta (type=THREAD_APPEND, prev_id=base.msg_id, payload="msg2").
3. Node B relays: if base arrived first, delta relays unchanged. If delta arrived first, repair requested → base re-sent → delta reconstructed.
4. Metric: `repairs_sent <= 1`, reconstruction success rate ≈ 100%.

#### Test 1C: TTL Clamping (2-node)
1. Node A sends chat (TTL=8).
2. Node B relays with TTL=7 (clamped by policy).
3. Metric: `next_ttl = min(received_ttl - 1, policy_max_ttl)`.

#### Test 1D: Relevance Decay (age-based suppression)
1. Node A sends status (priority=5, created_ms=now).
2. Wait 5 seconds.
3. Node A sends another message (so local relay happens at t=5s).
4. Node B should see relay attempt from A, but policy should suppress due to age.
5. Metric: `suppressed_reason == "AGE_EXCEEDS_PRIORITY"`.

### Test Topology: 3-Node Chain
```
Node A (RML) ──50m── Node B (RML) ──50m── Node C (RML)
```

**Goal**: Verify TTL decay, cumulative airtime, repair propagation.

#### Test 2A: TTL Decay Over 3 Hops
1. Node A sends chat (TTL=8).
2. Node B relays with TTL=7.
3. Node C relays with TTL=6 (if within range of B).
4. Metric: TTL decrements per-hop, stops at 0.

#### Test 2B: Repair Propagation
1. Intentionally drop packet on A→B link.
2. Node B detects missing prev_id, sends repair request upstream.
3. Node A receives repair (via C or direct), re-sends base.
4. Node B now reconstructs delta, relays to C.
5. Metric: repair latency ~1–2 seconds, bounded by cache size.

### Validation Script (pseudocode)
```bash
#!/bin/bash

# Flash 3 nodes
pio run -e <env> --target upload  # Repeat for each node

# Wait for boot
sleep 5

# Scenario 1: status dominance
send_rml_frame NodeA status "sender=${NODE_A_ID} state_type=1 payload=update1" pri=5
send_rml_frame NodeA status "sender=${NODE_A_ID} state_type=1 payload=update2" pri=5
send_rml_frame NodeA status "sender=${NODE_A_ID} state_type=1 payload=update3" pri=5

# Check Node B received exactly 1 status relay
relay_count=$(get_metric NodeB "rml_relayed" | grep -c "state_type=1")
if [ "$relay_count" -eq 1 ]; then
  echo "✅ Status dominance: passed (1 relay)"
else
  echo "❌ Status dominance: failed ($relay_count relays, expected 1)"
fi

# Scenario 2: delta reconstruction
base_id=$(send_rml_frame NodeA delta "type=append prev=0 payload=msg1")
delta_id=$(send_rml_frame NodeA delta "type=append prev=$base_id payload=msg2")

repair_count=$(get_metric NodeB "rml_repairs_sent")
if [ "$repair_count" -le 1 ]; then
  echo "✅ Delta reconstruction: passed ($repair_count repairs, expected ≤1)"
else
  echo "❌ Delta reconstruction: failed ($repair_count repairs)"
fi

echo "Multi-node validation complete."
```

---

## 11. UI/App Layer Integration (Future)

### Decision: Which RIVR Packets Use RML?

| Packet Type | Current | RML Candidate | Rationale |
|-------------|---------|--------------|-----------|
| Chat | Legacy | YES | benefits from delta chain, status dominance suppresses old convos |
| Status (sensor) | Legacy | YES | dominance eliminates stale readings, decay removes old values |
| Telemetry | Legacy | MAYBE | if compressible or time-series, else overhead not justified |
| Control/ACK | Legacy | NO | keep simple, real-time, no relay coalescing needed |
| Beacon | Legacy | NO | simple, frequent, no need for deltas |

### Proposed RML Send API
```c
// New API in rivr_layer/rivr_sources.c or app layer

// Send RML chat message
rml_send_chat(message_text, priority=10, ttl=8);

// Send RML status update (e.g., temperature sensor)
rml_send_status(state_type=TEMP_SENSOR, priority=5, payload_bytes, len);

// Send RML delta (e.g., firmware chunk)
rml_send_delta(thread_id, prev_msg_id, type=APPEND, payload_bytes, len);
```

### Implementation Path
1. **Phase 6 (proposed)**: Add RML send path in CLI (`rivr_cli.c`).
   - Command: `rml chat "text"`, `rml status state=temp value=25.3`, etc.
   - Call RML constructors, encode, submit to TX queue.

2. **Phase 7 (proposed)**: Integrate app layer.
   - Rivr script interpreter maps `send_chat()` → `rml_send_chat()` if RML enabled.
   - Fallback to legacy if RML disabled or recipient is legacy node.

3. **Phase 8 (proposed)**: UI display.
   - Show RML metadata (thread_id, state_type, repair count) on debug screen.
   - Relay statistics (suppressed, repaired) in metrics.

---

## 12. References & Next Steps

### Build & Test Commands
```bash
# Enable RML
export RIVR_ENABLE_RML=1

# Unit & sim tests
make -C tests clean rml

# Full test suite
make -C tests test

# Build firmware with RML
pio run -e client_esp32devkit_e22_900

# Deploy
pio run -e client_esp32devkit_e22_900 --target upload

# Hardware smoke test (see §7)
# Multi-node validation (see §10)
# RML frame injector (see §9)
```

### Related Documentation
- [firmware_core/rml/rml.h](../firmware_core/rml/rml.h): public API.
- [firmware_core/rml/rml_codec.c](../firmware_core/rml/rml_codec.c): wire format.
- [firmware_core/rml/rml_policy.c](../firmware_core/rml/rml_policy.c): policy engine.
- [tests/test_rml.c](../tests/test_rml.c): unit tests.
- [tests/test_rml_sim.c](../tests/test_rml_sim.c): simulation scenarios.
- [rivr_layer/rivr_sources.c](../rivr_layer/rivr_sources.c): RX dispatcher integration.

### Immediate Next Steps
1. **Hardware Smoke Test** (§7): flash 2 nodes, verify no breakage, logs clean.
2. **Debug Logging** (§8): enable policy decision logging, verify decisions are sensible.
3. **RML Frame Injector** (§9): add CLI commands for manual RML packet injection.
4. **Multi-Node Validation** (§10): 2–3 node topology, status dominance, delta reconstruction, repair bounds.
5. **App Layer Integration** (§11): decide which RIVR packets use RML, implement send API, update UI.

---

## 13. Appendix: Constants & Type Reference

### rml_message_type_t
```c
RML_TYPE_THREAD_MSG   = 0x01   // baseline message
RML_TYPE_STATUS       = 0x02   // status update (dominance rules)
RML_TYPE_DELTA        = 0x03   // append/replace reference
RML_TYPE_REPAIR       = 0x04   // repair request (missing-prev)
RML_TYPE_THREAD_SYNC  = 0x05   // reconstructed relay (policy-internal)
```

### rml_intent_t
```c
RML_INTENT_CHAT       = 0x01   // airtime floor: 50%
RML_INTENT_STATUS     = 0x02   // airtime floor: 30%
RML_INTENT_TELEMETRY  = 0x03   // airtime floor: 40%
RML_INTENT_CONTROL    = 0x04   // airtime floor: 80%
```

### rml_policy_decision_t (output)
```c
typedef struct {
    bool accept;                // true if message should be accepted locally
    bool relay;                 // true if should relay to neighbors
    uint16_t delay_ms;          // deterministic delay before relay
    uint8_t next_ttl;           // TTL for relayed packet
    uint8_t reason;             // suppression reason (AGE, DUTY, SEEN, etc.)
    uint16_t repair_delay_ms;   // if repair_needed, recommended delay
    bool forward_reconstructed; // true if delta was reconstructed on-the-fly
    bool repair_needed;         // true if prev_id missing (repair request needed)
    rml_repair_t repair;        // repair request details
} rml_policy_decision_t;
```

### Cache Limits
```c
RML_SEEN_CACHE_SIZE        = 64      // messages
RML_STATUS_CACHE_SIZE      = 16      // per-sender status entries
RML_THREAD_CACHE_SIZE      = 16      // thread entries
RML_REPAIR_CACHE_SIZE      = 8       // pending repairs
RML_MAX_PAYLOAD            = 160     // bytes
```

---

**Document Version**: 1.0 (Phase 5 Complete)  
**Last Updated**: 2026-05-04  
**Status**: Ready for Hardware Smoke Test
