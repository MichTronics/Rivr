//! # RIVR Engine
//!
//! The engine owns the stream-graph DAG and drives its execution.  It is
//! single-threaded, deterministic, and bounded – ideal for the ESP32 target.
//!
//! ## Execution model
//! 1. Caller injects events via [`Engine::inject`] (source events) or
//!    [`Engine::tick`] (advance logical time for time-based operators).
//! 2. The engine pushes each injected event to the scheduler.
//! 3. On [`Engine::run`], the scheduler is drained step by step:
//!    - Dequeue the next `(node_id, event)` work item.
//!    - Call `node.process(event, now_ms)`.
//!    - For each output event, push it to every downstream node's scheduler
//!      entry.
//! 4. Repeat until the scheduler is idle or `max_steps` is reached.
//!
//! ## Debug mode
//! Set [`Engine::debug`] to `true` to print a trace line for every event
//! processed, including the node name and logical timestamp.

use std::collections::HashMap;

use super::event::Event;
use super::node::{Node, NodeId};
use super::scheduler::Scheduler;
use super::value::Value;
use super::replay::ReplayLog;

// ─────────────────────────────────────────────────────────────────────────────

/// The RIVR stream-graph execution engine.
pub struct Engine {
    /// All nodes in the graph, indexed by their [`NodeId`].
    pub nodes: Vec<Node>,

    /// Maps source names (as declared with `source NAME = ...`) to their
    /// [`NodeId`] so callers can inject events by name.
    pub source_ids: HashMap<String, NodeId>,

    /// Maps all named streams (both sources and `let` bindings) to the
    /// [`NodeId`] whose output represents that stream.
    pub stream_ids: HashMap<String, NodeId>,

    /// FIFO work queue.
    pub scheduler: Scheduler,

    /// Current logical time in milliseconds.  Advances with each call to
    /// [`Engine::tick`] or automatically to match incoming event timestamps.
    pub now_ms: u64,

    /// When `true`, print a trace line to stderr for every processed event.
    pub debug: bool,

    /// Optional event log for deterministic replay.
    pub replay_log: Option<ReplayLog>,

    /// Total events processed (for statistics).
    pub events_processed: u64,
}

impl Engine {
    // ── Construction ─────────────────────────────────────────────────────

    /// Create an empty engine with no nodes.
    pub fn new() -> Self {
        Self {
            nodes:            Vec::new(),
            source_ids:       HashMap::new(),
            stream_ids:       HashMap::new(),
            scheduler:        Scheduler::new(),
            now_ms:            0,
            debug:             false,
            replay_log:        None,
            events_processed:  0,
        }
    }

    // ── Graph construction helpers ────────────────────────────────────────

    /// Allocate a new node, returning its [`NodeId`].
    pub fn add_node(&mut self, mut node: Node) -> NodeId {
        let id = self.nodes.len();
        node.id = id;
        self.nodes.push(node);
        id
    }

    /// Connect `upstream` → `downstream`: events produced by `upstream` are
    /// forwarded to `downstream`'s input queue.
    pub fn connect(&mut self, upstream: NodeId, downstream: NodeId) {
        self.nodes[upstream].outputs.push(downstream);
        self.nodes[downstream].inputs.push(upstream);
    }

    /// Register a source node so it can be injected by name.
    pub fn register_source(&mut self, name: impl Into<String>, id: NodeId) {
        let n = name.into();
        self.source_ids.insert(n.clone(), id);
        self.stream_ids.insert(n, id);
    }

    /// Register a named stream binding (`let NAME = ...`).
    pub fn register_stream(&mut self, name: impl Into<String>, id: NodeId) {
        self.stream_ids.insert(name.into(), id);
    }

    // ── Event injection ───────────────────────────────────────────────────

    /// Inject an event into the named source.
    ///
    /// The event is immediately pushed onto the scheduler.  The logical clock
    /// advances to `max(now_ms, ev.t_ms)`.
    ///
    /// If a replay log is set, the event is recorded before injection.
    ///
    /// # Errors
    /// Returns an error string if `source_name` does not exist.
    pub fn inject(&mut self, source_name: &str, ev: Event) -> Result<(), String> {
        let id = *self
            .source_ids
            .get(source_name)
            .ok_or_else(|| format!("unknown source `{source_name}`"))?;

        // Advance logical clock.
        if ev.t_ms > self.now_ms {
            self.now_ms = ev.t_ms;
        }

        // Record to replay log if active.
        if let Some(log) = &mut self.replay_log {
            let _ = log.record(source_name, &ev);
        }

        self.scheduler.push(id, ev);
        Ok(())
    }

    // ── Tick (advance logical time) ───────────────────────────────────────

    /// Advance the logical clock to `ms`, allowing time-based operators
    /// (debounce, throttle, window) to expire.
    ///
    /// This injects a synthetic `Value::Unit` event into every source so that
    /// time-based operators get a chance to fire.
    #[allow(dead_code)]
    pub fn tick(&mut self, ms: u64) {
        if ms <= self.now_ms {
            return;
        }
        self.now_ms = ms;
        // Inject a synthetic tick into every source.
        let source_ids: Vec<NodeId> = self.source_ids.values().cloned().collect();
        for id in source_ids {
            let ev = Event::new(ms, Value::Unit);
            self.scheduler.push(id, ev);
        }
    }

    // ── Main execution loop ───────────────────────────────────────────────

    /// Process up to `max_steps` work items from the scheduler.
    ///
    /// Returns the number of steps actually executed.  The caller can call
    /// `run` repeatedly; execution is resumable because state is in the nodes.
    pub fn run(&mut self, max_steps: usize) -> usize {
        let mut steps = 0;

        while steps < max_steps {
            let item = match self.scheduler.pop() {
                Some(i) => i,
                None    => break,
            };

            let now = self.now_ms;

            // Borrow the node mutably, process the event.
            let output_events = {
                let node = &mut self.nodes[item.node_id];
                if self.debug {
                    eprintln!(
                        "[trace] t={:>6}ms  node {:>2} ({})  ev={}",
                        item.event.t_ms, node.id, node.name, item.event
                    );
                }
                node.process(item.event, now)
            };

            self.events_processed += 1;

            // Fan out output events to all downstream nodes.
            let downstream: Vec<NodeId> = self.nodes[item.node_id].outputs.clone();
            for out_ev in output_events {
                for &ds_id in &downstream {
                    self.scheduler.push(ds_id, out_ev.clone());
                }
            }

            steps += 1;
        }

        steps
    }

    /// Run until the scheduler is idle.
    pub fn run_to_idle(&mut self) {
        while !self.scheduler.is_idle() {
            self.run(1024);
        }
    }

    // ── End-of-stream flush ───────────────────────────────────────────────

    /// Flush all time-based operators (windows, debounce) and process the
    /// resulting events.  Should be called when no more source events will
    /// arrive.
    pub fn flush_all(&mut self) {
        let now = self.now_ms;
        let node_count = self.nodes.len();
        for i in 0..node_count {
            let flush_events = self.nodes[i].flush(now);
            if !flush_events.is_empty() {
                let downstream = self.nodes[i].outputs.clone();
                for ev in flush_events {
                    for &ds_id in &downstream {
                        self.scheduler.push(ds_id, ev.clone());
                    }
                }
            }
        }
        self.run_to_idle();
    }

    // ── Statistics ────────────────────────────────────────────────────────

    /// Print a human-readable summary of engine statistics to stderr.
    pub fn print_stats(&self) {
        eprintln!("── RIVR Engine Statistics ────────────────────────");
        eprintln!("  Nodes            : {}", self.nodes.len());
        eprintln!("  Events processed : {}", self.events_processed);
        eprintln!("  Scheduler total  : {}", self.scheduler.total_enqueued);
        eprintln!("  Logical time     : {}ms", self.now_ms);
        let total_drops: u64 = self.nodes.iter().map(|n| n.drops).sum();
        eprintln!("  Total drops      : {total_drops}");
        eprintln!("──────────────────────────────────────────────────");
    }

    // ── Graph dump ────────────────────────────────────────────────────────

    /// Print the graph topology to stderr (useful for debugging the compiler).
    pub fn print_graph(&self) {
        eprintln!("── RIVR Stream Graph ─────────────────────────────");
        for node in &self.nodes {
            eprintln!(
                "  [{:>2}] {:<24} inputs={:?}  outputs={:?}",
                node.id, node.name, node.inputs, node.outputs
            );
        }
        eprintln!("──────────────────────────────────────────────────");
    }
}

impl Default for Engine {
    fn default() -> Self {
        Self::new()
    }
}
