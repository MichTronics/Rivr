//! # RIVR Stream Graph Nodes (v2)
//!
//! All operators compile to [`Node`]s connected in a DAG.
//! New in v2:
//! - All time-based operators use **ticks** drawn from `event.stamp.tick`.
//! - `WindowMs/ThrottleMs/DebounceMs` are kept as host conveniences but are
//!   identical to their `Ticks` siblings on clock 0.
//! - [`NodeKind::AirtimeBudget`] is the radio duty-cycle primitive.
//! - [`NodeKind::FilterKind`] discriminates on payload kind-tag.
//! - [`NodeKind::WindowTicks`] uses a [`BoundedVec`] to cap memory.

#[cfg(not(feature = "std"))]
use alloc::{boxed::Box, string::String, vec, vec::Vec};

use super::bounded::BoundedVec;
use super::event::{Event, Stamp};
use super::value::{StrBuf, Value};

// ─────────────────────────────────────────────────────────────────────────────
// Identifiers and policies
// ─────────────────────────────────────────────────────────────────────────────

pub type NodeId = usize;

/// Maximum events buffered in any node's window buffer.
pub const WINDOW_CAP: usize = 64;
/// Maximum events in any node's input queue.
pub const QUEUE_CAP: usize = 64;

/// Drop policy for bounded buffers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)]
pub enum DropPolicy {
    /// On overflow: evict the oldest event and accept the new one.
    DropOldest,
    /// On overflow: silently discard the new event.
    DropNewest,
    /// On overflow: flush the whole buffer as a `Window` event *before*
    /// accepting the new event (mid-window eager flush).
    FlushEarly,
}

// ─────────────────────────────────────────────────────────────────────────────
// Sink kinds
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub enum SinkKind {
    UsbPrint,
    LoraTx,
    LoraBeacon,
    DebugDump,
}

// ─────────────────────────────────────────────────────────────────────────────
// NodeKind
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug)]
pub enum NodeKind {
    // ── Sources ────────────────────────────────────────────────────────────
    /// Pass-through source node.  The engine injects events directly.
    Source {
        #[allow(dead_code)]
        name: String,
        /// The clock domain id assigned to this source at compile time.
        clock: u8,
        /// For `source NAME = timer(N)` declarations: the fire interval in
        /// milliseconds.  `None` for all other source kinds.
        /// Exposed to C via `rivr_foreach_timer_source()`.
        interval_ms: Option<u64>,
    },

    // ── Text / filter ──────────────────────────────────────────────────────
    MapUpper,
    /// Lowercase all `Str` payloads (ASCII).
    MapLower,
    /// Strip leading and trailing ASCII whitespace from `Str` payloads.
    MapTrim,
    FilterNonempty,
    /// Pass only events whose `Value::kind_tag()` equals `kind`.
    /// `kind == "*"` passes everything.
    FilterKind {
        kind: String,
    },

    /// Pass only `Value::Bytes` events where `bytes[3] == pkt_type`.
    ///
    /// Byte offset 3 is the `pkt_type` field of the RIVR binary wire format.
    /// All other value types are silently dropped.
    FilterPktType {
        pkt_type: u8,
    },

    // ── Aggregation ────────────────────────────────────────────────────────
    FoldCount {
        count: u64,
    },
    /// Running sum of `Int` payloads.  Emits the accumulated total.
    /// Non-integer events contribute 0 (not dropped).
    FoldSum {
        sum: i64,
    },
    /// Last-value latch.  Emits the most recently received `Value` on every
    /// event.  Before the first event it emits `Value::Unit`.
    FoldLast {
        last: Option<Value>,
    },

    // ── Tick-domain operators (canonical) ──────────────────────────────────
    /// Tumbling window keyed on `event.stamp.tick`.
    ///
    /// A window covers the tick range `[start, start+duration)`.  When an
    /// event arrives past the window boundary the old buffer is flushed as a
    /// `Value::Window` event and a new window starts.
    ///
    /// `cap` ≤ `WINDOW_CAP` limits the in-window buffer.  When
    /// `buffer_policy == FlushEarly`, reaching `cap` triggers an immediate
    /// mid-window flush rather than a drop.
    WindowTicks {
        duration: u64,
        /// Runtime capacity (may be < WINDOW_CAP).
        cap: usize,
        buffer: BoundedVec<Event, WINDOW_CAP>,
        window_start: u64,
        buffer_policy: DropPolicy,
    },

    /// Pass at most one event per `interval` ticks.
    ThrottleTicks {
        interval: u64,
        last_emit: u64, // u64::MAX = never emitted (first event always passes)
    },

    /// Forward the pending event only after `delay` ticks of silence.
    DelayTicks {
        delay: u64,
        pending: Option<(u64, Event)>, // (deadline_tick, event)
    },

    // ── Ms-domain aliases (host convenience; identical semantics) ──────────
    WindowMs {
        inner: Box<NodeKind>,
    }, // wraps WindowTicks on clock 0
    ThrottleMs {
        inner: Box<NodeKind>,
    },
    DebounceMs {
        inner: Box<NodeKind>,
    },

    // ── Rate limiting ──────────────────────────────────────────────────────
    Budget {
        rate_per_tick: f64, // tokens / tick
        burst: u64,
        tokens: f64,
        last_refill: u64,
    },

    /// **Airtime budget** – radio duty-cycle enforcer.
    ///
    /// Maintains a rolling window of `window_ticks` ticks.  Each forwarded
    /// event costs one "airtime unit".  Events are dropped once
    /// `used >= window_ticks * duty`.  The window resets automatically.
    AirtimeBudget {
        window_ticks: u64,
        /// Pre-computed: `(window_ticks as f64 * duty).floor() as u64`.
        budget_per_window: u64,
        /// Airtime units used in the current window.
        used: u64,
        /// Tick at which the current measurement window started.
        window_start: u64,
        /// Total dropped events (for diagnostics).
        total_dropped: u64,
    },

    /// **Time-on-Air budget** – radio duty-cycle enforcer using *actual*
    /// airtime cost per event rather than a unit count.
    ///
    /// Each forwarded event costs `toa_us_per_event` microseconds of airtime.
    /// Accumulated ToA in the rolling `window_ms`-tick window must stay
    /// below `window_ms * duty` milliseconds (converted to microseconds).
    ///
    /// Example:
    /// ```rivr
    /// source rf = rf;
    /// let safe = rf
    ///   |> budget.toa_us(window_ms=360000, duty=0.10, toa_us=400);
    /// emit { io.lora.tx(safe); }
    /// ```
    ToaBudget {
        /// Measurement window in ticks (== ms on clock 0).
        window_ms: u64,
        /// Maximum accumulated ToA per window in microseconds.
        budget_us: u64,
        /// Accumulated ToA used in the current window (microseconds).
        used_us: u64,
        /// Tick at which the current window started.
        window_start: u64,
        /// Fixed ToA cost per forwarded event (microseconds).
        toa_us_per_event: u64,
        /// Total events dropped (diagnostics).
        total_dropped: u64,
    },

    // ── Merge ──────────────────────────────────────────────────────────────
    Merge,

    // ── Debug ──────────────────────────────────────────────────────────────
    Tag {
        label: String,
    },

    // ── Sinks ──────────────────────────────────────────────────────────────
    Emit {
        sink: SinkKind,
    },
}

// ─────────────────────────────────────────────────────────────────────────────
// Node struct
// ─────────────────────────────────────────────────────────────────────────────

pub struct Node {
    pub id: NodeId,
    pub name: String,
    pub kind: NodeKind,
    pub inputs: Vec<NodeId>,
    pub outputs: Vec<NodeId>,
    /// Drops due to full input queue.
    pub drops: u64,
}

impl core::fmt::Debug for Node {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Node {{ id: {}, name: {:?} }}", self.id, self.name)
    }
}

impl Node {
    pub fn new(id: NodeId, name: impl Into<String>, kind: NodeKind) -> Self {
        Self {
            id,
            name: name.into(),
            kind,
            inputs: Vec::new(),
            outputs: Vec::new(),
            drops: 0,
        }
    }

    // ── process ───────────────────────────────────────────────────────────

    /// Process one incoming event and return the output events.
    /// `now_tick` is the *logical* tick on the event's clock domain.
    pub fn process(&mut self, ev: Event) -> Vec<Event> {
        let tick = ev.stamp.tick;

        match &mut self.kind {
            // ── Source ──────────────────────────────────────────────────
            NodeKind::Source { .. } => vec![ev],

            // ── Text / filter ────────────────────────────────────────────
            NodeKind::MapUpper => {
                let v = if let Value::Str(s) = ev.v {
                    Value::Str(s.to_ascii_uppercase())
                } else {
                    ev.v
                };
                vec![Event { v, ..ev }]
            }
            NodeKind::MapLower => {
                let v = if let Value::Str(s) = ev.v {
                    Value::Str(s.to_ascii_lowercase())
                } else {
                    ev.v
                };
                vec![Event { v, ..ev }]
            }
            NodeKind::MapTrim => {
                let v = if let Value::Str(s) = ev.v {
                    // AsRef<str> is ambiguous when StrBuf == String; cast through &str explicitly.
                    let trimmed: &str = (&s as &dyn core::ops::Deref<Target = str>).trim();
                    Value::Str(StrBuf::from(trimmed))
                } else {
                    ev.v
                };
                vec![Event { v, ..ev }]
            }
            NodeKind::FilterNonempty => {
                let keep = match &ev.v {
                    Value::Str(s) => !s.trim().is_empty(),
                    Value::Unit => false,
                    _ => true,
                };
                if keep {
                    vec![ev]
                } else {
                    vec![]
                }
            }
            NodeKind::FilterKind { kind } => {
                let k = kind.clone();
                let pass = k == "*" || ev.v.kind_tag() == k.as_str();
                if pass {
                    vec![ev]
                } else {
                    vec![]
                }
            }

            // ── filter.pkt_type ────────────────────────────────────────────
            NodeKind::FilterPktType { pkt_type } => {
                // PKT_TYPE_BYTE_OFFSET = 3 (matches the RIVR binary wire header)
                let pt = *pkt_type;
                let pass = match &ev.v {
                    Value::Bytes(b) => {
                        let slice: &[u8] = b.as_ref();
                        slice.len() > 3 && slice[3] == pt
                    }
                    _ => false,
                };
                if pass {
                    vec![ev]
                } else {
                    vec![]
                }
            }

            // ── fold.count ────────────────────────────────────────────────
            NodeKind::FoldCount { count } => {
                *count += 1;
                let n = *count;
                vec![Event::new(ev.stamp, Value::Int(n as i64))]
            }

            // ── fold.sum ──────────────────────────────────────────────────
            NodeKind::FoldSum { sum } => {
                if let Value::Int(n) = ev.v {
                    *sum += n;
                }
                let s = *sum;
                vec![Event::new(ev.stamp, Value::Int(s))]
            }

            // ── fold.last ─────────────────────────────────────────────────
            NodeKind::FoldLast { last } => {
                let out_val = last.clone().unwrap_or(Value::Unit);
                *last = Some(ev.v);
                vec![Event::new(ev.stamp, out_val)]
            }

            // ── window.ticks ──────────────────────────────────────────────
            NodeKind::WindowTicks {
                duration,
                cap,
                buffer,
                window_start,
                buffer_policy,
            } => {
                let dur = *duration;
                let ws = *window_start;
                let pol = *buffer_policy;
                let runtime_cap = *cap;
                let mut out = Vec::new();

                // ── Tick-boundary flush ──────────────────────────────────
                if tick >= ws + dur {
                    let flushed: Vec<String> = buffer
                        .drain_all()
                        .into_iter()
                        .map(|e| e.v.display())
                        .collect();
                    *window_start = (tick / dur) * dur;
                    if !flushed.is_empty() {
                        out.push(Event::new(ev.stamp, Value::Window(flushed)));
                    }
                }

                // ── FlushEarly: capacity reached before boundary ─────────
                if pol == DropPolicy::FlushEarly && buffer.is_full_capped(runtime_cap) {
                    let flushed: Vec<String> = buffer
                        .drain_all()
                        .into_iter()
                        .map(|e| e.v.display())
                        .collect();
                    if !flushed.is_empty() {
                        out.push(Event::new(ev.stamp, Value::Window(flushed)));
                    }
                }

                buffer.push_capped(ev, pol, runtime_cap);
                out
            }

            // ── Delegate ms aliases to their inner WindowTicks/etc ─────────
            NodeKind::WindowMs { inner }
            | NodeKind::ThrottleMs { inner }
            | NodeKind::DebounceMs { inner } => {
                // The inner node holds the real NodeKind (WindowTicks, etc.).
                // We just delegate.
                inner.process_inner(ev)
            }

            // ── throttle.ticks ────────────────────────────────────────────
            NodeKind::ThrottleTicks {
                interval,
                last_emit,
            } => {
                let ready = *last_emit == u64::MAX || tick >= last_emit.saturating_add(*interval);
                if ready {
                    *last_emit = tick;
                    vec![ev]
                } else {
                    vec![]
                }
            }

            // ── delay.ticks ───────────────────────────────────────────────
            NodeKind::DelayTicks { delay, pending } => {
                let d = *delay;
                let mut out = Vec::new();
                // Emit any pending event whose deadline has passed.
                if let Some((deadline, _)) = pending.as_ref() {
                    if tick >= *deadline {
                        let settled = pending.take().expect("just confirmed Some above").1;
                        out.push(settled);
                    }
                }
                // Park the new event.
                *pending = Some((tick + d, ev));
                out
            }

            // ── budget (token-bucket) ─────────────────────────────────────
            NodeKind::Budget {
                rate_per_tick,
                burst,
                tokens,
                last_refill,
            } => {
                let elapsed = tick.saturating_sub(*last_refill) as f64;
                *tokens = (*tokens + elapsed * *rate_per_tick).min(*burst as f64);
                *last_refill = tick;
                if *tokens >= 1.0 {
                    *tokens -= 1.0;
                    vec![ev]
                } else {
                    vec![]
                }
            }

            // ── airtime budget ────────────────────────────────────────────
            NodeKind::AirtimeBudget {
                window_ticks,
                budget_per_window,
                used,
                window_start,
                total_dropped,
            } => {
                let wt = *window_ticks;
                // Roll window if expired.
                if tick >= *window_start + wt {
                    let advance = (tick - *window_start) / wt;
                    *window_start += advance * wt;
                    *used = 0;
                }
                let budget = *budget_per_window;
                if *used < budget {
                    *used += 1;
                    vec![ev]
                } else {
                    *total_dropped += 1;
                    vec![]
                }
            }

            // ── budget.toa_us ─────────────────────────────────────────────
            NodeKind::ToaBudget {
                window_ms,
                budget_us,
                used_us,
                window_start,
                toa_us_per_event,
                total_dropped,
            } => {
                let wm = *window_ms;
                // Roll window on expiry
                if tick >= *window_start + wm {
                    let advance = (tick - *window_start) / wm;
                    *window_start += advance * wm;
                    *used_us = 0;
                }
                let cost = *toa_us_per_event;
                if *used_us + cost <= *budget_us {
                    *used_us += cost;
                    vec![ev]
                } else {
                    *total_dropped += 1;
                    vec![]
                }
            }

            // ── merge ─────────────────────────────────────────────────────
            NodeKind::Merge => vec![ev],

            // ── tag ───────────────────────────────────────────────────────
            NodeKind::Tag { label } => {
                let l = label.clone();
                vec![ev.with_tag(l)]
            }

            // ── emit (sink) ───────────────────────────────────────────────
            NodeKind::Emit { sink } => {
                #[cfg(feature = "ffi")]
                {
                    let sink_name = match sink {
                        SinkKind::UsbPrint => "io.usb.print",
                        SinkKind::LoraTx => "io.lora.tx",
                        SinkKind::LoraBeacon => "io.lora.beacon",
                        SinkKind::DebugDump => "io.debug.dump",
                    };
                    // Safety: called from the engine's single-threaded tick context;
                    // EMIT_DISPATCH is set once before engine init and never mutated.
                    unsafe {
                        crate::ffi::ffi_emit_hook(sink_name, ev.stamp, &ev.v, ev.tag.as_deref());
                    }
                }
                #[cfg(all(not(feature = "ffi"), feature = "std"))]
                match sink {
                    SinkKind::UsbPrint => println!("[usb] {}", ev.v.display()),
                    SinkKind::LoraBeacon => { /* dispatched via rivr_emit_dispatch in firmware */ }
                    SinkKind::LoraTx => {
                        let hex: String =
                            ev.v.display()
                                .bytes()
                                .map(|b| format!("{b:02x}"))
                                .collect::<Vec<_>>()
                                .join(" ");
                        println!("[lora] {} bytes: {hex}", ev.v.display().len());
                    }
                    SinkKind::DebugDump => eprintln!("[debug] {ev}"),
                }
                #[cfg(all(not(feature = "ffi"), not(feature = "std")))]
                let _ = sink;
                vec![Event::new(ev.stamp, Value::Unit)]
            }
        }
    }

    /// Flush any internally buffered state (end-of-stream / shutdown).
    pub fn flush(&mut self) -> Vec<Event> {
        match &mut self.kind {
            NodeKind::WindowTicks {
                buffer,
                window_start,
                ..
            } => {
                let ws = *window_start;
                let items: Vec<String> = buffer
                    .drain_all()
                    .into_iter()
                    .map(|e| e.v.display())
                    .collect();
                if items.is_empty() {
                    return vec![];
                }
                vec![Event::new(Stamp::mono(ws), Value::Window(items))]
            }
            NodeKind::WindowMs { inner } | NodeKind::DebounceMs { inner } => inner.flush_inner(),
            NodeKind::DelayTicks { pending, .. } => {
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

// ─────────────────────────────────────────────────────────────────────────────
// NodeKind helpers (process/flush on aliased inner nodes)
// ─────────────────────────────────────────────────────────────────────────────

impl NodeKind {
    fn process_inner(&mut self, ev: Event) -> Vec<Event> {
        match self {
            NodeKind::WindowTicks {
                duration,
                cap,
                buffer,
                window_start,
                buffer_policy,
            } => {
                let tick = ev.stamp.tick;
                let dur = *duration;
                let ws = *window_start;
                let pol = *buffer_policy;
                let runtime_cap = *cap;
                let mut out = Vec::new();

                if tick >= ws + dur {
                    let flushed: Vec<String> = buffer
                        .drain_all()
                        .into_iter()
                        .map(|e| e.v.display())
                        .collect();
                    *window_start = (tick / dur) * dur;
                    if !flushed.is_empty() {
                        out.push(Event::new(ev.stamp, Value::Window(flushed)));
                    }
                }
                if pol == DropPolicy::FlushEarly && buffer.is_full_capped(runtime_cap) {
                    let flushed: Vec<String> = buffer
                        .drain_all()
                        .into_iter()
                        .map(|e| e.v.display())
                        .collect();
                    if !flushed.is_empty() {
                        out.push(Event::new(ev.stamp, Value::Window(flushed)));
                    }
                }
                buffer.push_capped(ev, pol, runtime_cap);
                out
            }
            NodeKind::ThrottleTicks {
                interval,
                last_emit,
            } => {
                let tick = ev.stamp.tick;
                let ready = *last_emit == u64::MAX || tick >= last_emit.saturating_add(*interval);
                if ready {
                    *last_emit = tick;
                    vec![ev]
                } else {
                    vec![]
                }
            }
            NodeKind::DelayTicks { delay, pending } => {
                let tick = ev.stamp.tick;
                let d = *delay;
                let mut out = Vec::new();
                if let Some((deadline, _)) = pending.as_ref() {
                    if tick >= *deadline {
                        out.push(pending.take().expect("just confirmed Some above").1);
                    }
                }
                *pending = Some((tick + d, ev));
                out
            }
            // Shouldn't happen – aliased kinds only wrap the three above.
            other => {
                let _ = other;
                vec![]
            }
        }
    }

    fn flush_inner(&mut self) -> Vec<Event> {
        match self {
            NodeKind::WindowTicks {
                buffer,
                window_start,
                ..
            } => {
                let ws = *window_start;
                let items: Vec<String> = buffer
                    .drain_all()
                    .into_iter()
                    .map(|e| e.v.display())
                    .collect();
                if items.is_empty() {
                    return vec![];
                }
                vec![Event::new(Stamp::mono(ws), Value::Window(items))]
            }
            NodeKind::DelayTicks { pending, .. } => {
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
