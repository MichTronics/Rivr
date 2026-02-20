//! # RIVR Stream Graph Nodes
//!
//! Every operator in a RIVR program compiles to a [`Node`].  Nodes are
//! connected in a directed acyclic graph (DAG) by their [`NodeId`]s.
//!
//! The engine calls [`Node::process`] with a single incoming [`Event`] and
//! receives back a `Vec<Event>` of zero or more output events.  Nodes may
//! carry internal state (e.g. the current count of `fold.count`, or the token
//! level of `budget`) between calls.
//!
//! ## Bounded queues
//! Each node owns an input [`std::collections::VecDeque`] capped at
//! [`QUEUE_CAPACITY`].  When the queue is full the configured [`DropPolicy`]
//! decides whether to silently drop the oldest or the newest event.  This
//! prevents unbounded memory growth on slow embedded targets.

use std::collections::VecDeque;

use super::event::Event;
use super::value::Value;

// ─────────────────────────────────────────────────────────────────────────────
// Identifiers and policies
// ─────────────────────────────────────────────────────────────────────────────

/// Opaque identifier for a node in the [`super::engine::Engine`] DAG.
pub type NodeId = usize;

/// Default queue depth per node.  Low enough to be safe on embedded targets.
pub const QUEUE_CAPACITY: usize = 64;

/// What to do when a node's input queue is full.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)]
pub enum DropPolicy {
    /// Discard the oldest buffered event to make room for the new one.
    DropOldest,
    /// Discard the incoming event; keep the existing buffer intact.
    DropNewest,
}

// ─────────────────────────────────────────────────────────────────────────────
// Sink kinds (used by Emit nodes)
// ─────────────────────────────────────────────────────────────────────────────

/// The I/O action performed by an [`NodeKind::Emit`] node.
#[derive(Debug, Clone, PartialEq)]
pub enum SinkKind {
    /// Print each event to stdout (USB-serial simulation).
    UsbPrint,
    /// Transmit each event over LoRa (mocked – prints a hex dump).
    LoraTx,
    /// Dump detailed debug information for every event to stderr.
    DebugDump,
}

// ─────────────────────────────────────────────────────────────────────────────
// Node kinds (one per stream operator)
// ─────────────────────────────────────────────────────────────────────────────

/// The behaviour of a node, plus any operator-specific mutable state.
#[derive(Debug)]
pub enum NodeKind {
    // ── Sources ─────────────────────────────────────────────────────────
    /// Inert pass-through node.  Events are injected by the engine directly.
    #[allow(dead_code)]
    Source { name: String },

    // ── Transformation operators ─────────────────────────────────────────
    /// `map.upper()` – uppercase text payloads.
    MapUpper,
    /// `filter.nonempty()` – drop events with empty-or-whitespace strings.
    FilterNonempty,
    /// `fold.count()` – emit a running count [`Value::Int`] on each event.
    FoldCount { count: u64 },

    // ── Time-based operators ─────────────────────────────────────────────
    /// `window.ms(N)` – collect events into a buffer and flush it every N ms.
    WindowMs {
        duration_ms: u64,
        buffer: Vec<Event>,
        /// Logical time at which the current window started.
        window_start: u64,
    },
    /// `throttle.ms(N)` – pass at most one event per N-ms interval.
    ThrottleMs {
        interval_ms: u64,
        /// Logical time of the last forwarded event.
        last_emit_ms: u64,
    },
    /// `debounce.ms(N)` – forward the last event only after N ms of silence.
    DebounceMs {
        delay_ms: u64,
        /// Pending (deadline_ms, event) waiting to be emitted.
        pending: Option<(u64, Event)>,
    },

    // ── Rate-limiting ────────────────────────────────────────────────────
    /// `budget(rate, burst)` – token-bucket rate limiter.
    ///
    /// | parameter | meaning                            |
    /// |-----------|------------------------------------|
    /// | `rate`    | tokens replenished per millisecond |
    /// | `burst`   | maximum token level (bucket size)  |
    Budget {
        rate_per_ms: f64,
        burst: u64,
        tokens: f64,
        last_refill_ms: u64,
    },

    // ── Merge ────────────────────────────────────────────────────────────
    /// `merge(a, b)` – forwards events from *either* upstream unchanged.
    Merge,

    // ── Debug / tracing ──────────────────────────────────────────────────
    /// `tag("label")` – attach a trace label to every event.
    Tag { label: String },

    // ── Sinks ────────────────────────────────────────────────────────────
    /// `emit { sink; }` – perform a side-effectful I/O action.
    Emit { sink: SinkKind },
}

// ─────────────────────────────────────────────────────────────────────────────
// Node struct
// ─────────────────────────────────────────────────────────────────────────────

/// A node in the stream graph.
///
/// The engine accesses nodes by their [`id`] (an index into the engine's
/// `nodes` vector).  Connections between nodes are modelled as `outputs`
/// (the downstream node ids to fan out events to).
pub struct Node {
    /// Unique identity of this node within the engine.
    pub id: NodeId,

    /// Human-readable name, used in debug / trace output.
    pub name: String,

    /// Operator kind plus any mutable operator-specific state.
    pub kind: NodeKind,

    /// IDs of nodes that feed events *into* this node.
    pub inputs: Vec<NodeId>,

    /// IDs of nodes that receive events *from* this node.
    pub outputs: Vec<NodeId>,

    /// Bounded FIFO input queue.
    pub queue: VecDeque<Event>,

    /// Maximum queue depth before the drop policy fires.
    pub queue_capacity: usize,

    /// What to do when the queue is full.
    pub drop_policy: DropPolicy,

    /// Count of events dropped due to a full queue.
    pub drops: u64,
}

impl std::fmt::Debug for Node {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "Node {{ id: {}, name: {:?}, inputs: {:?}, outputs: {:?} }}",
            self.id, self.name, self.inputs, self.outputs
        )
    }
}

impl Node {
    /// Construct a new node with default queue settings.
    pub fn new(id: NodeId, name: impl Into<String>, kind: NodeKind) -> Self {
        Self {
            id,
            name: name.into(),
            kind,
            inputs: Vec::new(),
            outputs: Vec::new(),
            queue: VecDeque::with_capacity(QUEUE_CAPACITY),
            queue_capacity: QUEUE_CAPACITY,
            drop_policy: DropPolicy::DropOldest,
            drops: 0,
        }
    }

    // ── Queue management ─────────────────────────────────────────────────

    /// Enqueue an event, applying the drop policy if the queue is full.
    #[allow(dead_code)]
    pub fn enqueue(&mut self, ev: Event) {
        if self.queue.len() >= self.queue_capacity {
            self.drops += 1;
            match self.drop_policy {
                DropPolicy::DropOldest => {
                    self.queue.pop_front();
                    self.queue.push_back(ev);
                }
                DropPolicy::DropNewest => {
                    // discard incoming event; buffer unchanged
                }
            }
        } else {
            self.queue.push_back(ev);
        }
    }

    /// Dequeue the next event from the input queue, if any.
    #[allow(dead_code)]
    pub fn dequeue(&mut self) -> Option<Event> {
        self.queue.pop_front()
    }

    // ── Event processing ─────────────────────────────────────────────────

    /// Process a single incoming event and return the output events to
    /// propagate downstream.
    ///
    /// `now_ms` is the *logical* current time supplied by the engine.  It may
    /// equal `ev.t_ms` for normal forward execution, or any past value during
    /// replay.
    pub fn process(&mut self, ev: Event, now_ms: u64) -> Vec<Event> {
        match &mut self.kind {
            // ── Sources ──────────────────────────────────────────────────
            NodeKind::Source { .. } => {
                // Sources pass events through unchanged.
                vec![ev]
            }

            // ── map.upper ────────────────────────────────────────────────
            NodeKind::MapUpper => {
                let v = match ev.v {
                    Value::Str(s) => Value::Str(s.to_ascii_uppercase()),
                    other => other,
                };
                vec![Event { v, ..ev }]
            }

            // ── filter.nonempty ───────────────────────────────────────────
            NodeKind::FilterNonempty => {
                let keep = match &ev.v {
                    Value::Str(s) => !s.trim().is_empty(),
                    Value::Unit   => false,
                    _             => true,
                };
                if keep { vec![ev] } else { vec![] }
            }

            // ── fold.count ────────────────────────────────────────────────
            NodeKind::FoldCount { count } => {
                *count += 1;
                let n = *count;
                vec![Event::new(ev.t_ms, Value::Int(n as i64))]
            }

            // ── window.ms ─────────────────────────────────────────────────
            NodeKind::WindowMs { duration_ms, buffer, window_start } => {
                // If the event falls outside the current window, flush first.
                let dur = *duration_ms;
                let ws  = *window_start;

                if ev.t_ms >= ws + dur {
                    // Flush the existing window.
                    let flushed: Vec<String> = buffer
                        .drain(..)
                        .map(|e| e.v.display())
                        .collect();
                    // Reset window start to align with the new event.
                    *window_start = (ev.t_ms / dur) * dur;
                    // Buffer the new event.
                    buffer.push(ev.clone());
                    if flushed.is_empty() {
                        // Nothing to emit – the window was empty.
                        vec![]
                    } else {
                        vec![Event::new(ws + dur, Value::Window(flushed))]
                    }
                } else {
                    buffer.push(ev);
                    vec![]
                }
            }

            // ── throttle.ms ───────────────────────────────────────────────
            // `last_emit_ms == u64::MAX` is the sentinel for "never emitted",
            // which ensures the very first event always passes through.
            NodeKind::ThrottleMs { interval_ms, last_emit_ms } => {
                let ready = *last_emit_ms == u64::MAX
                    || ev.t_ms >= last_emit_ms.saturating_add(*interval_ms);
                if ready {
                    *last_emit_ms = ev.t_ms;
                    vec![ev]
                } else {
                    vec![]
                }
            }

            // ── debounce.ms ───────────────────────────────────────────────
            // NOTE: in the stream-graph model, debounce is tricky because the
            // graph processes events synchronously.  We simulate it with a
            // "pending" slot: when a new event arrives we update the pending
            // slot; if the *next* event that arrives is from a synthetic
            // "timer" tick (or there simply are no more events within
            // `delay_ms` of the pending one), we emit it.
            NodeKind::DebounceMs { delay_ms, pending } => {
                let delay = *delay_ms;
                // Check if the previously pending event has now "settled".
                let mut out = Vec::new();
                if let Some((deadline, _)) = pending {
                    if ev.t_ms >= *deadline {
                        // Emit the settled event.
                        let settled = pending.take().unwrap().1;
                        out.push(settled);
                    }
                }
                // Replace the pending slot with the new event.
                *pending = Some((ev.t_ms + delay, ev));
                out
            }

            // ── budget ────────────────────────────────────────────────────
            NodeKind::Budget { rate_per_ms, burst, tokens, last_refill_ms } => {
                // Refill tokens based on elapsed logical time.
                let elapsed = ev.t_ms.saturating_sub(*last_refill_ms) as f64;
                *tokens = (*tokens + elapsed * *rate_per_ms).min(*burst as f64);
                *last_refill_ms = ev.t_ms;

                if *tokens >= 1.0 {
                    *tokens -= 1.0;
                    vec![ev]
                } else {
                    vec![] // drop – over budget
                }
            }

            // ── merge ─────────────────────────────────────────────────────
            NodeKind::Merge => {
                // Both upstream nodes connect to this node's input; we just
                // pass events through.
                vec![ev]
            }

            // ── tag ───────────────────────────────────────────────────────
            NodeKind::Tag { label } => {
                let l = label.clone();
                vec![ev.with_tag(l)]
            }

            // ── emit (sink) ───────────────────────────────────────────────
            NodeKind::Emit { sink } => {
                match sink {
                    SinkKind::UsbPrint => {
                        println!("[usb] {}", ev.v.display());
                    }
                    SinkKind::LoraTx => {
                        let payload = ev.v.display();
                        let hex: String = payload
                            .as_bytes()
                            .iter()
                            .map(|b| format!("{b:02x}"))
                            .collect::<Vec<_>>()
                            .join(" ");
                        println!("[lora tx] {} bytes: {hex}", payload.len());
                    }
                    SinkKind::DebugDump => {
                        eprintln!("[debug] {ev}");
                    }
                }
                // Sinks produce a `Unit` event so the engine can count them.
                vec![Event::new(now_ms, Value::Unit)]
            }
        }
    }

    /// Force-flush any internally buffered events (called at end-of-stream or
    /// at engine shutdown).
    pub fn flush(&mut self, _now_ms: u64) -> Vec<Event> {
        match &mut self.kind {
            NodeKind::WindowMs { buffer, window_start, .. } => {
                let ws = *window_start;
                let items: Vec<String> = buffer
                    .drain(..)
                    .map(|e| e.v.display())
                    .collect();
                if items.is_empty() {
                    vec![]
                } else {
                    vec![Event::new(ws, Value::Window(items))]
                }
            }
            NodeKind::DebounceMs { pending, .. } => {
                if let Some((_, ev)) = pending.take() {
                    vec![ev]
                } else {
                    vec![]
                }
            }
            _ => vec![],
        }
    }
}
