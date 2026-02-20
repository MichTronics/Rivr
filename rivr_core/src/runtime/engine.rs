//! # RIVR Engine (v2)
//!
//! Changes over v1:
//! - Tracks a global `seq_counter` assigned at injection → feeds `event.seq`.
//! - `inject` accepts an `Event` whose stamp already carries the clock domain.
//! - `tick_clock(clock, tick)` advances a specific clock domain.
//! - `run` uses the priority scheduler (ordered by `(clock, tick, seq, node_id)`).

#[cfg(not(feature = "std"))]
use alloc::{collections::BTreeMap, string::String, vec::Vec, format};
#[cfg(feature = "std")]
use std::collections::HashMap;

use super::event::{Event, Stamp};
use super::node::{Node, NodeId, NodeKind, SinkKind};

// ─────────────────────────────────────────────────────────────────────────────
// Emit effect record (for replay / testing)
// ─────────────────────────────────────────────────────────────────────────────

/// A single emit side-effect captured while the engine runs.
///
/// Stored in [`Engine::effect_log`] when enabled via
/// [`Engine::enable_effect_log`].  Used by Replay 2.0 to assert that a
/// replay reproduces exactly the same output trace.
#[derive(Debug, Clone, PartialEq)]
pub struct EffectRecord {
    pub stamp:     Stamp,
    pub sink:      SinkKind,
    /// `Display` rendering of the event value at the time of emission.
    pub value_str: String,
}
use super::scheduler::Scheduler;
use super::value::Value;

// ─────────────────────────────────────────────────────────────────────────────

/// Built-in clock name → id mapping.
pub const CLOCK_MONO: u8 = 0;
pub const CLOCK_LMP:  u8 = 1;
pub const CLOCK_GPS:  u8 = 2;

pub fn clock_id(name: &str) -> u8 {
    match name {
        "mono" | "ms" => CLOCK_MONO,
        "lmp"         => CLOCK_LMP,
        "gps"         => CLOCK_GPS,
        _             => 0,   // unknown → default to mono
    }
}

// ─────────────────────────────────────────────────────────────────────────────

pub struct Engine {
    pub nodes:        Vec<Node>,

    #[cfg(feature = "std")]
    pub source_ids:   HashMap<String, NodeId>,
    #[cfg(feature = "std")]
    pub stream_ids:   HashMap<String, NodeId>,

    #[cfg(not(feature = "std"))]
    pub source_ids:   BTreeMap<String, NodeId>,
    #[cfg(not(feature = "std"))]
    pub stream_ids:   BTreeMap<String, NodeId>,

    pub scheduler:         Scheduler,
    /// Current logical tick per clock domain.  Index = clock id.
    pub clock_now:         [u64; 8],
    pub debug:             bool,
    pub seq_counter:       u32,
    pub events_processed:  u64,
    /// Optional effect capture for Replay 2.0.  `None` = disabled.
    pub effect_log:        Option<Vec<EffectRecord>>,
}

impl Engine {
    pub fn new() -> Self {
        Self {
            nodes:             Vec::new(),
            #[cfg(feature = "std")]
            source_ids:        HashMap::new(),
            #[cfg(feature = "std")]
            stream_ids:        HashMap::new(),
            #[cfg(not(feature = "std"))]
            source_ids:        BTreeMap::new(),
            #[cfg(not(feature = "std"))]
            stream_ids:        BTreeMap::new(),
            scheduler:         Scheduler::new(),
            clock_now:         [0; 8],
            debug:             false,
            seq_counter:       0,
            events_processed:  0,
            effect_log:        None,
        }
    }

    // ── Graph construction ────────────────────────────────────────────────

    pub fn add_node(&mut self, mut node: Node) -> NodeId {
        let id = self.nodes.len();
        node.id = id;
        self.nodes.push(node);
        id
    }

    pub fn connect(&mut self, up: NodeId, down: NodeId) {
        self.nodes[up].outputs.push(down);
        self.nodes[down].inputs.push(up);
    }

    pub fn register_source(&mut self, name: impl Into<String>, id: NodeId) {
        let n = name.into();
        self.source_ids.insert(n.clone(), id);
        self.stream_ids.insert(n, id);
    }

    pub fn register_stream(&mut self, name: impl Into<String>, id: NodeId) {
        self.stream_ids.insert(name.into(), id);
    }

    // ── Inject ────────────────────────────────────────────────────────────

    /// Inject an event into a named source node.
    ///
    /// The engine stamps the event's sequence number and advances the logical
    /// clock for the event's clock domain.
    pub fn inject(&mut self, source_name: &str, mut ev: Event) -> Result<(), String> {
        let id = *self.source_ids.get(source_name)
            .ok_or_else(|| format!("unknown source `{source_name}`"))?;

        let clk = ev.stamp.clock as usize;
        if clk < self.clock_now.len() && ev.stamp.tick > self.clock_now[clk] {
            self.clock_now[clk] = ev.stamp.tick;
        }

        ev.seq = self.seq_counter;
        self.seq_counter = self.seq_counter.wrapping_add(1);

        self.scheduler.push(id, ev);
        Ok(())
    }

    /// Inject a simple mono-clock event by timestamp and string value.
    pub fn inject_str(&mut self, source: &str, ms: u64, text: impl Into<String>) -> Result<(), String> {
        self.inject(source, Event::mono(ms, Value::Str(text.into())))
    }

    // ── Tick ──────────────────────────────────────────────────────────────

    /// Advance clock `clock_id` to `tick`, injecting a synthetic `Unit`
    /// event into every source on that clock so time-based operators fire.
    pub fn tick_clock(&mut self, clock_id: u8, tick: u64) {
        if (clock_id as usize) < self.clock_now.len() {
            if tick <= self.clock_now[clock_id as usize] { return; }
            self.clock_now[clock_id as usize] = tick;
        }
        let stamp = Stamp::at(clock_id, tick);
        // Collect sources whose assigned clock matches.
        let matching: Vec<NodeId> = self.nodes.iter()
            .filter(|n| matches!(&n.kind, super::node::NodeKind::Source { clock, .. } if *clock == clock_id))
            .map(|n| n.id)
            .collect();
        for id in matching {
            let mut ev = Event::new(stamp, Value::Unit);
            ev.seq = self.seq_counter;
            self.seq_counter = self.seq_counter.wrapping_add(1);
            self.scheduler.push(id, ev);
        }
    }

    // ── Run ───────────────────────────────────────────────────────────────

    /// Process up to `max_steps` work items.  Returns the count executed.
    pub fn run(&mut self, max_steps: usize) -> usize {
        let mut steps = 0;
        while steps < max_steps {
            let item = match self.scheduler.pop() {
                Some(i) => i,
                None    => break,
            };

            if self.debug {
                let n = &self.nodes[item.node_id];
                eprintln!(
                    "[trace] clk{}:tick{:<8} seq={:<6} node {:>2} ({})  ev={}",
                    item.event.stamp.clock, item.event.stamp.tick,
                    item.event.seq, n.id, n.name, item.event
                );
            }

            // ── Capture emit effect before processing ───────────────────────
            if let Some(log) = &mut self.effect_log {
                if let NodeKind::Emit { sink } = &self.nodes[item.node_id].kind {
                    log.push(EffectRecord {
                        stamp:     item.event.stamp,
                        sink:      sink.clone(),
                        value_str: item.event.v.display(),
                    });
                }
            }

            let output_events = self.nodes[item.node_id].process(item.event);
            self.events_processed += 1;

            let downstream: Vec<NodeId> = self.nodes[item.node_id].outputs.clone();
            for mut out_ev in output_events {
                out_ev.seq = self.seq_counter;
                self.seq_counter = self.seq_counter.wrapping_add(1);
                for &ds_id in &downstream {
                    self.scheduler.push(ds_id, out_ev.clone());
                }
            }
            steps += 1;
        }
        steps
    }

    pub fn run_to_idle(&mut self) {
        while !self.scheduler.is_idle() { self.run(4096); }
    }

    pub fn flush_all(&mut self) {
        let node_count = self.nodes.len();
        for i in 0..node_count {
            let flush_events = self.nodes[i].flush();
            if !flush_events.is_empty() {
                let downstream = self.nodes[i].outputs.clone();
                for mut ev in flush_events {
                    ev.seq = self.seq_counter;
                    self.seq_counter = self.seq_counter.wrapping_add(1);
                    for &ds_id in &downstream {
                        self.scheduler.push(ds_id, ev.clone());
                    }
                }
            }
        }
        self.run_to_idle();
    }

    // ── Effect log (Replay 2.0) ───────────────────────────────────────────

    /// Enable effect capture.  All subsequent [`Emit`](crate::runtime::node::NodeKind::Emit)
    /// events will be appended to the log.
    pub fn enable_effect_log(&mut self) {
        self.effect_log = Some(Vec::new());
    }

    /// Disable effect capture and return all recorded effects, leaving the
    /// log empty.
    pub fn take_effect_log(&mut self) -> Vec<EffectRecord> {
        self.effect_log.take().unwrap_or_default()
    }

    // ── Stats / diagnostics ───────────────────────────────────────────────

    pub fn print_stats(&self) {
        eprintln!("── RIVR Engine Statistics ─────────────────────────");
        eprintln!("  Nodes            : {}", self.nodes.len());
        eprintln!("  Events processed : {}", self.events_processed);
        eprintln!("  Sched total      : {}", self.scheduler.total_enqueued);
        for (i, &t) in self.clock_now.iter().enumerate() {
            if t > 0 { eprintln!("  Clock[{i}] now     : {t}"); }
        }
        let pending = self.scheduler.clock_pending_counts();
        for (i, &p) in pending.iter().enumerate() {
            if p > 0 { eprintln!("  Clock[{i}] pending : {p}"); }
        }
        let drops: u64 = self.nodes.iter().map(|n| n.drops).sum();
        eprintln!("  Queue drops      : {drops}");
        eprintln!("───────────────────────────────────────────────────");
    }

    pub fn print_graph(&self) {
        eprintln!("── RIVR Stream Graph ──────────────────────────────");
        for n in &self.nodes {
            eprintln!("  [{:>2}] {:<28} in={:?}  out={:?}", n.id, n.name, n.inputs, n.outputs);
        }
        eprintln!("───────────────────────────────────────────────────");
    }
}

impl Default for Engine {
    fn default() -> Self { Self::new() }
}
