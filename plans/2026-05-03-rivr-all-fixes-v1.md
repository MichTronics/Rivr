# Rivr — All Identified Fixes

## Objective

Apply all bug fixes and quality improvements identified during the code review, radio interop diagnosis, and relay gate analysis. The three radio fixes (PreamblePolarity, AFC enable, AFC BW register) are already committed and are listed here for completeness only.

---

## Status Key

| Symbol | Meaning |
|---|---|
| ✅ | Already done |
| - [ ] | Not yet done |

---

## Implementation Plan

### Group A — Radio Interop Fixes (already done)

- [x] **A1. PreamblePolarity register** — `firmware_core/radio_sx1276.c:379`
  Changed `0xB1u` → `0x91u`. Aligns LilyGo SX1276 TX preamble to 0xAA so E22 SX1262 preamble detector locks correctly. Root cause of `lnk_cnt:0`.

- [x] **A2. AFC enabled for COMPAT_10K profile** — `firmware_core/rivr_fsk_profile.c:33`
  Changed `afc_enabled = false` → `afc_enabled = true`. Allows SX1276 to correct the TCXO-to-crystal carrier offset on packets from the E22.

- [x] **A3. AFC bandwidth register** — `firmware_core/radio_sx1276.c:369`
  Changed `0x08u` (invalid encoding) → `0x01u` (250 kHz, wider than the 200 kHz RX BW). Required for AFC to have enough pull-in range.

---

### Group B — Correctness Bugs

- [ ] **B1. Stale callsign/role on neighbor slot eviction** — `firmware_core/routing.c:169`

  `neighbor_fill_slot()` does not zero the slot before writing new fields. When a slot is evicted and reused for a different node, the new entry inherits the previous occupant's `callsign[]` and `role` until that node sends a beacon. This produces wrong display output and can affect routing decisions.

  Fix: add `memset(n, 0, sizeof(*n));` as the first line of `neighbor_fill_slot()`, before any field assignments.

- [ ] **B2. No fallback when NVS stores an invalid program** — `rivr_layer/rivr_embed.c:358-368`

  The existing fallback to compiled-in defaults only fires when `nvs_load_program()` returns `NULL`. If NVS holds a non-null but syntactically invalid string (e.g. corrupted OTA push), `rivr_engine_init()` fails and the device enters a watchdog-bait infinite loop with no recovery path other than a forced reflash.

  Fix: after the initial `rivr_engine_init()` call, check whether `rc.code != RIVR_OK` and `prog_src != s_policy_buf`. If so, log a warning, rebuild from `rivr_policy_build_program()`, and retry `rivr_engine_init()` with the default program. Only reach the final infinite loop when even the compiled-in default fails (which is a genuine firmware defect).

- [ ] **B3. ToA estimate truncates for non-power-of-2 bandwidths** — `firmware_core/routing.h:531`

  The preamble term `49u * t_sym / 4u` truncates when `t_sym % 4 != 0`. This is exact for all currently configured bandwidths (all powers of 2), making it a latent bug. If `RF_BANDWIDTH_HZ` is ever set to a non-standard value such as 41700 Hz, duty-cycle decisions will use a slightly-low ToA estimate, potentially violating the regulatory budget.

  Fix: reorder as `(49u * t_sym + 2u) / 4u` to round rather than truncate. The change is a single line in the static inline function.

---

### Group C — Policy / Relay Gate Fix

- [ ] **C1. Policy token bucket count too small** — `firmware_core/policy.h:54`

  `POLICY_BUCKET_COUNT = 16` means all source node IDs are hashed into only 16 slots via `src_id % 16`. Two unrelated nodes that share the same bucket also share the same token pool. In a mesh with more than ~8 actively chatting nodes, hash collisions become frequent and legitimate single chat relays are silently dropped.

  Fix: raise `POLICY_BUCKET_COUNT` from `16u` to `64u`. This reduces collision probability to acceptable levels for typical mesh deployments without changing the struct layout significantly (`policy_state_t` grows from 80 B to 320 B — still trivial BSS cost). No changes to `policy.c` logic are needed; the index expression `hdr->src_id % POLICY_BUCKET_COUNT` at `policy.c:64` adapts automatically.

  If a larger mesh (>50 nodes) is anticipated, consider 128 as an upper bound; beyond that a direct per-source table or a hash map with open addressing would be more appropriate.

---

### Group D — Code Quality Improvements

- [ ] **D1. Compiler warnings dispatched before node-limit check** — `rivr_core/src/ffi.rs:254-281`

  The `warns` dispatch loop fires before the `RIVR_MAX_NODES` guard. A program that is too large will emit warning callbacks for an engine that is then rejected. The C caller receives diagnostics for a program it will never run.

  Fix: move the `engine.nodes.len() > RIVR_MAX_NODES` check to immediately after `compile()` returns and before the dispatch loop. If the limit is exceeded, return `RIVR_ERR_NODE_LIMIT` without emitting any warnings.

- [ ] **D2. Dead `loop_guard` hash computation** — `firmware_core/routing.c:457-475`

  `my_h` is computed and immediately discarded with `(void)my_h`. The feature was intentionally disabled (valid reasoning documented in the comment), but the dead computation misleads anyone reading the forward path. Future readers may assume it has a side effect.

  Fix: either remove the four lines entirely (`my_h` declaration, `my_id != 0u` check, `routing_loop_guard_hash()` call, `(void)my_h` cast), or gate the entire block behind `#if RIVR_FEATURE_LOOP_GUARD` so re-enablement is a single compile-time flag.

- [ ] **D3. Module-wide `static_mut_refs` suppression** — `rivr_core/src/ffi.rs:3`

  `#![allow(static_mut_refs)]` silences the Rust 2024 lint for the entire module. The actual sites that require it are a small number of `assume_init_mut()` / `assume_init_ref()` calls inside `unsafe` blocks. The broad suppression hides any future accidental static mutable reference additions anywhere in the module.

  Fix: remove the module-level `#![allow(...)]` attribute. Switch read-only accesses to `addr_of!(ENGINE_SLOT)` where possible. Apply `#[allow(static_mut_refs)]` only to the specific `unsafe` blocks that call `assume_init_mut()` and `assume_init_ref()`.

---

## Verification Criteria

- After B1: provision a mesh with ≥17 nodes, let the table fill and evict; verify no stale callsign appears in the neighbor display for the new occupant before its first beacon.
- After B2: write a syntactically invalid UTF-8 string to NVS via the OTA path; verify the device logs a warning and boots successfully on the compiled-in default rather than hanging.
- After B3: set `RF_BANDWIDTH_HZ` to 41700 in a test build; verify `routing_toa_estimate_us()` returns a value within 1% of the reference SX126x ToA calculator output.
- After C1: replay a scenario with 20 distinct source node IDs all sending one chat message within 4 seconds; verify all 20 are relayed (none dropped by the policy gate).
- After D1: build a `.rivr` program that exceeds `RIVR_MAX_NODES`; verify no warning callbacks fire on the C side before the `RIVR_ERR_NODE_LIMIT` error is returned.
- After D2: confirm the forward path compiles cleanly with no unused-variable warnings and the comment accurately describes the disabled feature.
- After D3: confirm `cargo clippy` reports zero `static_mut_refs` warnings in `ffi.rs`; confirm the module still compiles on both `no_std` embedded and Linux host targets.

---

## Potential Risks and Mitigations

1. **B2 fallback re-init alters boot observable state**
   If other subsystems read engine state between the first failed `rivr_engine_init()` and the retry, they may observe a partially-initialised engine. Mitigation: ensure `rivr_engine_freeze()` is never called before the retry succeeds, and that no tasks are unblocked from the engine init semaphore until the final successful call.

2. **C1 BSS growth**
   Raising `POLICY_BUCKET_COUNT` to 64 grows `policy_state_t` from 80 B to 320 B. On constrained variants (e.g. boards with only 4 KB free BSS) this may require audit of the overall static allocation budget. Mitigation: run `idf.py size` before and after on the tightest board variant.

3. **D3 refactoring introduces unsafe scope errors**
   Narrowing `allow(static_mut_refs)` may reveal other sites in the module that were relying on the broad suppression without being obvious. Mitigation: apply the change incrementally — remove the module attribute first, fix each compiler error one by one, then switch applicable reads to `addr_of!`.

---

## Execution Order

The fixes are independent and can be applied in any order, but this sequence minimises risk:

1. C1 (policy bucket — smallest blast radius, single constant change)
2. B3 (ToA rounding — single line, self-contained)
3. B1 (memset — one line addition, isolated function)
4. B2 (NVS fallback — most impactful, test in a staging build first)
5. D2 (dead code removal — cosmetic, easy to verify)
6. D1 (ffi.rs warn ordering — affects diagnostic UX only)
7. D3 (lint scope narrowing — most mechanical, needs incremental compile verification)
