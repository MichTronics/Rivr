//! # RIVR Replay 2.0 (host-only)
//!
//! ## Trace format (JSONL)
//! Each line is a JSON object of one of two shapes:
//!
//! **Source event (prefix `"in"`):**
//! ```json
//! {"kind":"in","source":"usb","stamp":{"clock":0,"tick":100},"v":"hello"}
//! ```
//!
//! **Emit effect (prefix `"out"`):**
//! ```json
//! {"kind":"out","sink":"usb_print","stamp":{"clock":0,"tick":500},"v":"[window 3 events]"}
//! ```
//!
//! ## Operation modes
//! | mode            | what happens                                                |
//! |-----------------|-------------------------------------------------------------|
//! | **record**      | inject events, capture effects, write JSONL trace file      |
//! | **replay**      | feed JSONL events back into engine, collect effects         |
//! | **assert**      | replay + compare effect trace against recorded effects       |

use serde::{Deserialize, Serialize};
use rivr_core::{Engine, Event, Stamp, Value};
use rivr_core::runtime::{engine::EffectRecord, node::SinkKind};

// ─────────────────────────────────────────────────────────────────────────────
// JSON wire types
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Serialize, Deserialize)]
pub(crate) struct StampJson { pub clock: u8, pub tick: u64 }

#[derive(Serialize, Deserialize)]
#[serde(untagged)]
pub(crate) enum ValueJson {
    Float(f64),
    Int(i64),
    Str(String),
    Bool(bool),
}

/// A single JSONL trace record (input event **or** output effect).
#[derive(Serialize, Deserialize)]
pub struct TraceRecord {
    pub kind:    String,          // "in" or "out"
    /// For "in": the source name.  For "out": the sink kind string.
    pub channel: String,
    pub stamp:   StampJson,
    pub v:       ValueJson,
}

// ─────────────────────────────────────────────────────────────────────────────
// Conversion helpers
// ─────────────────────────────────────────────────────────────────────────────

fn value_json_to_value(v: &ValueJson) -> Value {
    match v {
        ValueJson::Float(f) => {
            let i = *f as i64;
            if (i as f64 - f).abs() < 1e-9 { Value::Int(i) } else { Value::Str(format!("{f}")) }
        }
        ValueJson::Int(i)  => Value::Int(*i),
        ValueJson::Str(s)  => Value::Str(s.clone()),
        ValueJson::Bool(b) => Value::Bool(*b),
    }
}

fn value_to_json(v: &Value) -> ValueJson {
    match v {
        Value::Int(i)       => ValueJson::Int(*i),
        Value::Str(s)       => ValueJson::Str(s.clone()),
        Value::Bool(b)      => ValueJson::Bool(*b),
        Value::Window(w)    => ValueJson::Str(format!("[window {} events]", w.len())),
        _                   => ValueJson::Str("[unit]".into()),
    }
}

fn sink_kind_str(s: &SinkKind) -> &'static str {
    match s {
        SinkKind::UsbPrint  => "usb_print",
        SinkKind::LoraTx    => "lora_tx",
        SinkKind::DebugDump => "debug_dump",
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Record: produce a JSONL trace from a live run
// ─────────────────────────────────────────────────────────────────────────────

/// Feed events into the engine WITH effect capture, and return the JSONL trace
/// as a `String` (ready to be written to a file or compared later).
pub fn record(engine: &mut Engine, events: &[(&str, Event)]) -> String {
    engine.enable_effect_log();
    for (source, ev) in events {
        let _ = engine.inject(source, ev.clone());
    }
    engine.run_to_idle();
    let effects = engine.take_effect_log();

    // Serialise input events
    let mut lines: Vec<String> = events.iter().map(|(src, ev)| {
        let rec = TraceRecord {
            kind:    "in".into(),
            channel: src.to_string(),
            stamp:   StampJson { clock: ev.stamp.clock, tick: ev.stamp.tick },
            v:       value_to_json(&ev.v),
        };
        serde_json::to_string(&rec).unwrap_or_default()
    }).collect();

    // Serialise output effects
    for eff in &effects {
        let rec = TraceRecord {
            kind:    "out".into(),
            channel: sink_kind_str(&eff.sink).to_string(),
            stamp:   StampJson { clock: eff.stamp.clock, tick: eff.stamp.tick },
            v:       ValueJson::Str(eff.value_str.clone()),
        };
        lines.push(serde_json::to_string(&rec).unwrap_or_default());
    }
    lines.join("\n")
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay: feed a JSONL trace back into the engine
// ─────────────────────────────────────────────────────────────────────────────

/// Parse and inject all `"in"` records from a JSONL trace into the engine.
/// Returns the effects produced during this replay run.
pub fn replay(engine: &mut Engine, content: &str) -> Vec<EffectRecord> {
    println!("\n═══ REPLAY ════════════════════════════════\n");
    engine.enable_effect_log();

    let mut n_in = 0usize;
    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') { continue; }

        match serde_json::from_str::<TraceRecord>(line) {
            Ok(rec) if rec.kind == "in" => {
                let v     = value_json_to_value(&rec.v);
                let stamp = Stamp::at(rec.stamp.clock, rec.stamp.tick);
                let ev    = Event { stamp, v, tag: None, seq: 0 };
                let _ = engine.inject(&rec.channel, ev);
                n_in += 1;
            }
            Ok(_)     => { /* skip "out" records */ }
            Err(e)    => eprintln!("WARN: skipping malformed line: {e}"),
        }
    }

    engine.run(10_000);
    let effects = engine.take_effect_log();
    println!("── replay complete: {n_in} events injected, {} effects ──\n", effects.len());
    engine.print_stats();
    effects
}

// ─────────────────────────────────────────────────────────────────────────────
// Assert: replay + compare effect traces
// ─────────────────────────────────────────────────────────────────────────────

/// Replay a trace and assert that the reproduced effects exactly match the
/// effects in the `"out"` records stored in the trace.
///
/// Returns `Ok(())` on a matching trace, `Err(mismatches)` otherwise.
pub fn replay_and_assert(engine: &mut Engine, content: &str) -> Result<(), Vec<String>> {
    // Collect expected effects from "out" records in the trace.
    let expected: Vec<(String, String, u64)> = content.lines()
        .filter_map(|l| serde_json::from_str::<TraceRecord>(l.trim()).ok())
        .filter(|r| r.kind == "out")
        .map(|r| {
        let v_str = match &r.v {
            ValueJson::Str(s) => s.clone(),
            ValueJson::Int(n) => n.to_string(),
            ValueJson::Float(f) => f.to_string(),
            ValueJson::Bool(b) => b.to_string(),
        };
            (r.channel.clone(), v_str, r.stamp.tick)
        })
        .collect();

    let actual_effects = replay(engine, content);
    let actual: Vec<(String, String, u64)> = actual_effects.iter()
        .map(|e| (sink_kind_str(&e.sink).to_string(), e.value_str.clone(), e.stamp.tick))
        .collect();

    if expected == actual {
        println!("✓ replay assertion PASSED ({} effects match)\n", expected.len());
        return Ok(());
    }

    let mut mismatches = Vec::new();
    let n = expected.len().max(actual.len());
    for i in 0..n {
        let exp = expected.get(i);
        let act = actual.get(i);
        if exp != act {
            mismatches.push(format!(
                "  effect[{i}]: expected {:?}  got {:?}",
                exp, act
            ));
        }
    }
    println!("✗ replay assertion FAILED ({} mismatches):", mismatches.len());
    for m in &mismatches { println!("{m}"); }
    Err(mismatches)
}

// ─────────────────────────────────────────────────────────────────────────────
// Simple JSONL replay (legacy compat, used by --replay flag)
// ─────────────────────────────────────────────────────────────────────────────

/// Legacy replay: accepts both old-style `{"source":...}` lines and new-style
/// `{"kind":"in",...}` lines.
pub fn replay_legacy(engine: &mut Engine, content: &str) {
    println!("\n═══ REPLAY (legacy) ════════════════════════════════\n");
    let mut n = 0usize;
    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') { continue; }

        // Try new format first
        if let Ok(rec) = serde_json::from_str::<TraceRecord>(line) {
            if rec.kind == "in" {
                let v     = value_json_to_value(&rec.v);
                let stamp = Stamp::at(rec.stamp.clock, rec.stamp.tick);
                let ev    = Event { stamp, v, tag: None, seq: 0 };
                let _ = engine.inject(&rec.channel, ev);
                n += 1;
                continue;
            }
        }
        // Try legacy {source, stamp, v} format
        #[derive(Deserialize)]
        struct LegacyEntry { source: String, stamp: StampJson, v: ValueJson }
        if let Ok(e) = serde_json::from_str::<LegacyEntry>(line) {
            let v     = value_json_to_value(&e.v);
            let stamp = Stamp::at(e.stamp.clock, e.stamp.tick);
            let ev    = Event { stamp, v, tag: None, seq: 0 };
            let _ = engine.inject(&e.source, ev);
            n += 1;
        } else {
            eprintln!("WARN: skipping malformed line");
        }
    }
    engine.run(1_000);
    println!("── legacy replay complete: {n} events injected ──\n");
    engine.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Capture helper: build a JSONL record string (used when logging manually)
// ─────────────────────────────────────────────────────────────────────────────

#[allow(dead_code)]
pub fn log_entry(source: &str, stamp: Stamp, v: &Value) -> String {
    let rec = TraceRecord {
        kind:    "in".into(),
        channel: source.to_string(),
        stamp:   StampJson { clock: stamp.clock, tick: stamp.tick },
        v:       value_to_json(v),
    };
    serde_json::to_string(&rec).unwrap_or_default()
}
