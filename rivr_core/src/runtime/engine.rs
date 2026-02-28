//! # RIVR Engine (v2)
//!
//! Changes over v1:
//! - Tracks a global `seq_counter` assigned at injection → feeds `event.seq`.
//! - `inject` accepts an `Event` whose stamp already carries the clock domain.
//! - `tick_clock(clock, tick)` advances a specific clock domain.
//! - `run` uses the priority scheduler (ordered by `(clock, tick, seq, node_id)`).

#[cfg(all(not(feature = "std"), feature = "alloc"))]
use alloc::{collections::BTreeMap, format, string::String, vec, vec::Vec};
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
    pub stamp: Stamp,
    pub sink: SinkKind,
    /// `Display` rendering of the event value at the time of emission.
    pub value_str: String,
}

// ─────────────────────────────────────────────────────────────────────────────
// Trace record  (tag() operator → @TRACE log line)
// ─────────────────────────────────────────────────────────────────────────────

/// A single execution trace entry, produced when an event passes through
/// a [`NodeKind::Tag`](super::node::NodeKind::Tag) node.
///
/// Retrieve with [`Engine::take_trace_log`] on the host side.
#[derive(Debug, Clone, PartialEq)]
pub struct TraceRecord {
    /// The label passed to `tag("...")` in the RIVR program.
    pub label: String,
    /// Logical timestamp of the event.
    pub stamp: Stamp,
    /// Name of the Tag node in the compiled graph.
    pub node_name: String,
    /// Human-readable value at trace time.
    pub value_str: String,
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-source metrics
// ─────────────────────────────────────────────────────────────────────────────

/// Per-source injection statistics tracked by the engine.
#[derive(Debug, Clone, Default)]
pub struct SourceMetrics {
    /// Total successful [`Engine::inject`] calls for this source.
    pub injected: u64,
    /// Lamport tick of the most recently injected event (0 = none).
    pub last_tick: u64,
    /// Clock domain of the most recently injected event.
    pub last_clock: u8,
}
use super::scheduler::Scheduler;
use super::value::Value;

// ─────────────────────────────────────────────────────────────────────────────

/// Built-in clock name → id mapping.
pub const CLOCK_MONO: u8 = 0;
pub const CLOCK_LMP: u8 = 1;
pub const CLOCK_GPS: u8 = 2;

pub fn clock_id(name: &str) -> u8 {
    match name {
        "mono" | "ms" => CLOCK_MONO,
        "lmp" => CLOCK_LMP,
        "gps" => CLOCK_GPS,
        _ => 0, // unknown → default to mono
    }
}

// ─────────────────────────────────────────────────────────────────────────────

pub struct Engine {
    pub nodes: Vec<Node>,

    #[cfg(feature = "std")]
    pub source_ids: HashMap<String, NodeId>,
    #[cfg(feature = "std")]
    pub stream_ids: HashMap<String, NodeId>,

    #[cfg(not(feature = "std"))]
    pub source_ids: BTreeMap<String, NodeId>,
    #[cfg(not(feature = "std"))]
    pub stream_ids: BTreeMap<String, NodeId>,

    pub scheduler: Scheduler,
    /// Current logical tick per clock domain.  Index = clock id.
    pub clock_now: [u64; 8],
    pub debug: bool,
    pub seq_counter: u32,
    pub events_processed: u64,
    /// Optional effect capture for Replay 2.0.  `None` = disabled.
    pub effect_log: Option<Vec<EffectRecord>>,
    /// Optional trace capture for `tag()` operator.  `None` = disabled.
    pub trace_log: Option<Vec<TraceRecord>>,
    /// Per-source injection counters.  Key = source name.
    pub source_metrics: Vec<(String, SourceMetrics)>,
}

impl Engine {
    pub fn new() -> Self {
        Self {
            nodes: Vec::new(),
            #[cfg(feature = "std")]
            source_ids: HashMap::new(),
            #[cfg(feature = "std")]
            stream_ids: HashMap::new(),
            #[cfg(not(feature = "std"))]
            source_ids: BTreeMap::new(),
            #[cfg(not(feature = "std"))]
            stream_ids: BTreeMap::new(),
            scheduler: Scheduler::new(),
            clock_now: [0; 8],
            debug: false,
            seq_counter: 0,
            events_processed: 0,
            effect_log: None,
            trace_log: None,
            source_metrics: vec![],
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
        let id = *self
            .source_ids
            .get(source_name)
            .ok_or_else(|| format!("unknown source `{source_name}`"))?;

        let clk = ev.stamp.clock as usize;
        if clk < self.clock_now.len() && ev.stamp.tick > self.clock_now[clk] {
            self.clock_now[clk] = ev.stamp.tick;
        }

        // Update per-source metrics.
        match self
            .source_metrics
            .iter_mut()
            .find(|(k, _)| k == source_name)
        {
            Some((_, m)) => {
                m.injected += 1;
                m.last_tick = ev.stamp.tick;
                m.last_clock = ev.stamp.clock;
            }
            None => {
                self.source_metrics.push((
                    String::from(source_name),
                    SourceMetrics {
                        injected: 1,
                        last_tick: ev.stamp.tick,
                        last_clock: ev.stamp.clock,
                    },
                ));
            }
        }

        ev.seq = self.seq_counter;
        self.seq_counter = self.seq_counter.wrapping_add(1);

        self.scheduler.push(id, ev);
        Ok(())
    }

    /// Inject a simple mono-clock event by timestamp and string value.
    pub fn inject_str(
        &mut self,
        source: &str,
        ms: u64,
        text: impl Into<String>,
    ) -> Result<(), String> {
        self.inject(source, Event::mono(ms, Value::Str(text.into())))
    }

    // ── Tick ──────────────────────────────────────────────────────────────

    /// Advance clock `clock_id` to `tick`, injecting a synthetic `Unit`
    /// event into every source on that clock so time-based operators fire.
    pub fn tick_clock(&mut self, clock_id: u8, tick: u64) {
        if (clock_id as usize) < self.clock_now.len() {
            if tick <= self.clock_now[clock_id as usize] {
                return;
            }
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
                None => break,
            };

            if self.debug {
                let n = &self.nodes[item.node_id];
                eprintln!(
                    "[trace] clk{}:tick{:<8} seq={:<6} node {:>2} ({})  ev={}",
                    item.event.stamp.clock,
                    item.event.stamp.tick,
                    item.event.seq,
                    n.id,
                    n.name,
                    item.event
                );
            }

            // ── Capture emit effect before processing ───────────────────────
            if let Some(log) = &mut self.effect_log {
                if let NodeKind::Emit { sink } = &self.nodes[item.node_id].kind {
                    log.push(EffectRecord {
                        stamp: item.event.stamp,
                        sink: sink.clone(),
                        value_str: item.event.v.display(),
                    });
                }
            }

            // ── Pre-extract Tag label before process() borrows node ─────────
            // We need the label (owned) before calling process(), because
            // process() takes `ev` by value and mutably borrows the node.
            let trace_tag_label: Option<(String, String)> = if self.trace_log.is_some() {
                if let NodeKind::Tag { label } = &self.nodes[item.node_id].kind {
                    Some((label.clone(), self.nodes[item.node_id].name.clone()))
                } else {
                    None
                }
            } else {
                None
            };

            let output_events = self.nodes[item.node_id].process(item.event);
            self.events_processed += 1;

            // ── Capture trace records ───────────────────────────────────────
            if let (Some((label, node_name)), Some(log)) =
                (trace_tag_label, self.trace_log.as_mut())
            {
                for ev in &output_events {
                    log.push(TraceRecord {
                        label: label.clone(),
                        stamp: ev.stamp,
                        node_name: node_name.clone(),
                        value_str: ev.v.display(),
                    });
                }
            }

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
        while !self.scheduler.is_idle() {
            self.run(4096);
        }
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

    // ── Trace log (tag() operator) ────────────────────────────────────────

    /// Enable trace capture.  After this call, every event that passes
    /// through a `tag("label")` node will produce a [`TraceRecord`] in the
    /// log.
    pub fn enable_trace_log(&mut self) {
        self.trace_log = Some(Vec::new());
    }

    /// Drain and return all captured trace records, disabling future capture.
    pub fn take_trace_log(&mut self) -> Vec<TraceRecord> {
        self.trace_log.take().unwrap_or_default()
    }

    // ── Per-source metrics ────────────────────────────────────────────────

    /// Return an immutable view of per-source injection statistics.
    pub fn source_metrics(&self) -> &[(String, SourceMetrics)] {
        &self.source_metrics
    }

    /// Produce a compact JSON object string for all source metrics.
    ///
    /// Example output (single line):
    /// ```text
    /// {"rf_rx":{"injected":42,"last_tick":1234,"last_clock":1}}
    /// ```
    #[cfg(any(feature = "std", feature = "alloc"))]
    pub fn source_metrics_json(&self) -> String {
        let mut out = String::from('{');
        for (i, (name, m)) in self.source_metrics.iter().enumerate() {
            if i > 0 {
                out.push(',');
            }
            out.push_str(&format!(
                "\"{}\":{{\"injected\":{},\"last_tick\":{},\"last_clock\":{}}}",
                name, m.injected, m.last_tick, m.last_clock
            ));
        }
        out.push('}');
        out
    }

    // ── Stats / diagnostics ───────────────────────────────────────────────

    pub fn print_stats(&self) {
        eprintln!("── RIVR Engine Statistics ─────────────────────────");
        eprintln!("  Nodes            : {}", self.nodes.len());
        eprintln!("  Events processed : {}", self.events_processed);
        eprintln!("  Sched total      : {}", self.scheduler.total_enqueued);
        for (i, &t) in self.clock_now.iter().enumerate() {
            if t > 0 {
                eprintln!("  Clock[{i}] now     : {t}");
            }
        }
        let pending = self.scheduler.clock_pending_counts();
        for (i, &p) in pending.iter().enumerate() {
            if p > 0 {
                eprintln!("  Clock[{i}] pending : {p}");
            }
        }
        let drops: u64 = self.nodes.iter().map(|n| n.drops).sum();
        eprintln!("  Queue drops      : {drops}");
        eprintln!("───────────────────────────────────────────────────");
    }

    pub fn print_graph(&self) {
        eprintln!("── RIVR Stream Graph ──────────────────────────────");
        for n in &self.nodes {
            eprintln!(
                "  [{:>2}] {:<28} in={:?}  out={:?}",
                n.id, n.name, n.inputs, n.outputs
            );
        }
        eprintln!("───────────────────────────────────────────────────");
    }
}

impl Default for Engine {
    fn default() -> Self {
        Self::new()
    }
}
