//! # RIVR Compiler (v2)
//!
//! Walks the [`Program`] AST and builds a stream graph inside an [`Engine`].
//!
//! ## v2 additions
//! - Clock resolution: `source NAME @mono = usb;` → assigns `clock: u8` to the
//!   source node.  All events injected through that source get the matching
//!   clock domain in their `Stamp`.
//! - `WindowMs / ThrottleMs / DebounceMs` compile to `WindowTicks /
//!   ThrottleTicks / DelayTicks` on `clock 0` (mono) so tick-logic is unified
//!   in the runtime.
//! - `budget.airtime(window_ticks, duty)` compiles to `NodeKind::AirtimeBudget`.
//! - `filter.kind("TAG")` compiles to `NodeKind::FilterKind`.
//! - Static type-check pass validates sources, clocks, and emit bindings.

#[cfg(not(feature = "std"))]
use alloc::{collections::BTreeMap, string::{String, ToString}, vec::Vec, format};
#[cfg(feature = "std")]
use std::collections::HashMap;

use crate::ast::*;
use crate::runtime::engine::clock_id;
use crate::runtime::{
    bounded::BoundedVec,
    engine::Engine,
    node::{DropPolicy, Node, NodeId, NodeKind, SinkKind, WINDOW_CAP},
};

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug)]
pub struct CompileError {
    pub kind:    ErrorKind,
    pub message: String,
}

#[derive(Debug, PartialEq)]
pub enum ErrorKind { Error, Warning }

impl core::fmt::Display for CompileError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let tag = if self.kind == ErrorKind::Error { "ERROR" } else { "WARN" };
        write!(f, "[{tag}] {}", self.message)
    }
}

pub type CompileResult = Result<(Engine, Vec<CompileError>), Vec<CompileError>>;

// ─────────────────────────────────────────────────────────────────────────────
// Compiler state
// ─────────────────────────────────────────────────────────────────────────────

struct Compiler {
    engine:     Engine,
    #[cfg(feature = "std")]
    bindings:   HashMap<String, NodeId>,
    #[cfg(not(feature = "std"))]
    bindings:   BTreeMap<String, NodeId>,
    diags:      Vec<CompileError>,
    next_anon:  usize,
}

impl Compiler {
    fn new() -> Self {
        Self {
            engine:    Engine::new(),
            #[cfg(feature = "std")]
            bindings:  HashMap::new(),
            #[cfg(not(feature = "std"))]
            bindings:  BTreeMap::new(),
            diags:     Vec::new(),
            next_anon: 0,
        }
    }

    fn fresh(&mut self, prefix: &str) -> String {
        let n = self.next_anon;
        self.next_anon += 1;
        format!("{prefix}#{n}")
    }

    fn error(&mut self, msg: impl Into<String>) {
        self.diags.push(CompileError { kind: ErrorKind::Error, message: msg.into() });
    }
    fn warn(&mut self, msg: impl Into<String>) {
        self.diags.push(CompileError { kind: ErrorKind::Warning, message: msg.into() });
    }
    fn resolve(&self, name: &str) -> Option<NodeId> {
        self.bindings.get(name).copied()
    }

    // ── Pipe-op → NodeKind ────────────────────────────────────────────────

    fn compile_op(&mut self, op: &PipeOp) -> (String, NodeKind) {
        match op {
            PipeOp::MapUpper       => ("map.upper".into(),      NodeKind::MapUpper),
            PipeOp::FilterNonempty => ("filter.nonempty".into(), NodeKind::FilterNonempty),
            PipeOp::FoldCount      => ("fold.count".into(),      NodeKind::FoldCount { count: 0 }),

            PipeOp::FilterKind(k)  => (
                format!("filter.kind({})", k),
                NodeKind::FilterKind { kind: k.clone() },
            ),            PipeOp::FilterPktType(t) => (
                format!("filter.pkt_type({})", t),
                NodeKind::FilterPktType { pkt_type: *t },
            ),
            // ── Ms operators → alias inner WindowTicks/ThrottleTicks/DelayTicks ──
            PipeOp::WindowMs(ms) => (
                format!("window.ms({ms})"),
                NodeKind::WindowMs { inner: Box::new(NodeKind::WindowTicks {
                    duration:      *ms,
                    cap:           WINDOW_CAP,
                    buffer:        BoundedVec::new(),
                    window_start:  0,
                    buffer_policy: DropPolicy::DropOldest,
                })},
            ),
            PipeOp::ThrottleMs(ms) => (
                format!("throttle.ms({ms})"),
                NodeKind::ThrottleMs { inner: Box::new(NodeKind::ThrottleTicks {
                    interval:  *ms,
                    last_emit: u64::MAX,
                })},
            ),
            PipeOp::DebounceMs(ms) => (
                format!("debounce.ms({ms})"),
                NodeKind::DebounceMs { inner: Box::new(NodeKind::DelayTicks {
                    delay:   *ms,
                    pending: None,
                })},
            ),

            // ── Tick-domain ───────────────────────────────────────────────
            PipeOp::WindowTicks(n) => (
                format!("window.ticks({n})"),
                NodeKind::WindowTicks {
                    duration:      *n,
                    cap:           WINDOW_CAP,
                    buffer:        BoundedVec::new(),
                    window_start:  0,
                    buffer_policy: DropPolicy::DropOldest,
                },
            ),
            PipeOp::WindowTicksCapped { ticks, cap, policy } => {
                use crate::ast::WindowPolicy;
                let dp = match policy {
                    WindowPolicy::DropOldest => DropPolicy::DropOldest,
                    WindowPolicy::DropNewest => DropPolicy::DropNewest,
                    WindowPolicy::FlushEarly => DropPolicy::FlushEarly,
                };
                let runtime_cap = (*cap).min(WINDOW_CAP);
                (
                    format!("window.ticks({ticks},{cap},{policy:?})"),
                    NodeKind::WindowTicks {
                        duration:      *ticks,
                        cap:           runtime_cap,
                        buffer:        BoundedVec::new(),
                        window_start:  0,
                        buffer_policy: dp,
                    },
                )
            }
            PipeOp::ThrottleTicks(n) => (
                format!("throttle.ticks({n})"),
                NodeKind::ThrottleTicks { interval: *n, last_emit: u64::MAX },
            ),
            PipeOp::DelayTicks(n) => (
                format!("delay.ticks({n})"),
                NodeKind::DelayTicks { delay: *n, pending: None },
            ),

            // ── Rate limiting ─────────────────────────────────────────────
            PipeOp::Budget { rate, burst } => (
                format!("budget({rate:.2})"),
                NodeKind::Budget {
                    rate_per_tick: rate / 1000.0,
                    burst:         *burst,
                    tokens:        *burst as f64,
                    last_refill:   0,
                },
            ),
            PipeOp::BudgetAirtime { window_ticks, duty } => {
                let budget_per_window = (*window_ticks as f64 * duty).floor() as u64;
                (
                    format!("budget.airtime({window_ticks},{duty:.3})"),
                    NodeKind::AirtimeBudget {
                        window_ticks:      *window_ticks,
                        budget_per_window,
                        used:              0,
                        window_start:      0,
                        total_dropped:     0,
                    },
                )
            }
            PipeOp::BudgetToaUs { window_ms, duty, toa_us } => {
                let budget_us = (*window_ms as f64 * duty * 1_000.0).floor() as u64;
                (
                    format!("budget.toa_us({window_ms},{duty:.3},{toa_us}us)"),
                    NodeKind::ToaBudget {
                        window_ms:        *window_ms,
                        budget_us,
                        used_us:          0,
                        window_start:     0,
                        toa_us_per_event: *toa_us,
                        total_dropped:    0,
                    },
                )
            }

            PipeOp::Tag(label) => (
                format!("tag({})", label),
                NodeKind::Tag { label: label.clone() },
            ),
        }
    }

    // ── Expression compilation ────────────────────────────────────────────

    fn compile_expr(&mut self, expr: &Expr) -> Option<NodeId> {
        match expr {
            Expr::Ident(name) => {
                match self.resolve(name) {
                    Some(id) => Some(id),
                    None => { self.error(format!("undefined stream `{name}`")); None }
                }
            }
            Expr::Lit(_) => {
                self.warn("standalone literals are not yet supported as stream expressions");
                None
            }
            Expr::Pipe { lhs, op } => {
                let upstream = self.compile_expr(lhs)?;
                let (name, kind) = self.compile_op(op);
                let node_name = self.fresh(&name);
                let node = Node::new(self.engine.nodes.len(), node_name, kind);
                let id   = self.engine.add_node(node);
                self.engine.connect(upstream, id);
                Some(id)
            }
            Expr::Merge(a, b) => {
                let id_a = match self.resolve(a) { Some(x) => x, None => { self.error(format!("undefined stream `{a}` in merge")); return None; } };
                let id_b = match self.resolve(b) { Some(x) => x, None => { self.error(format!("undefined stream `{b}` in merge")); return None; } };
                let name = self.fresh("merge");
                let node = Node::new(self.engine.nodes.len(), name, NodeKind::Merge);
                let id   = self.engine.add_node(node);
                self.engine.connect(id_a, id);
                self.engine.connect(id_b, id);
                Some(id)
            }
        }
    }

    // ── Statement compilation ─────────────────────────────────────────────

    fn compile_stmt(&mut self, stmt: &Stmt) {
        match stmt {
            Stmt::Source { name, clock, kind } => {
                // Resolve clock id.
                let clk_id: u8 = clock
                    .as_ref()
                    .map(|c| clock_id(&c.name))
                    .unwrap_or(0); // default: mono

                let node_kind = NodeKind::Source { name: name.clone(), clock: clk_id };
                let node      = Node::new(self.engine.nodes.len(), name.clone(), node_kind);
                let id        = self.engine.add_node(node);
                self.engine.register_source(name.clone(), id);
                self.bindings.insert(name.clone(), id);

                if matches!(kind, SourceKind::Programmatic) {
                    self.warn(format!("source `{name}` is programmatic – events must be injected by the host"));
                }
            }

            Stmt::Let { name, expr } => {
                if let Some(terminal) = self.compile_expr(expr) {
                    self.engine.nodes[terminal].name = name.clone();
                    self.engine.register_stream(name.clone(), terminal);
                    self.bindings.insert(name.clone(), terminal);
                }
            }

            Stmt::Emit { sinks } => {
                for sink in sinks {
                    let (stream_name, sink_kind) = match sink {
                        Sink::UsbPrint(n)  => (n, SinkKind::UsbPrint),
                        Sink::LoraTx(n)    => (n, SinkKind::LoraTx),
                        Sink::DebugDump(n) => (n, SinkKind::DebugDump),
                    };
                    let upstream = match self.resolve(stream_name) {
                        Some(id) => id,
                        None => { self.error(format!("emit references undefined stream `{stream_name}`")); continue; }
                    };
                    let node_name = format!("emit({})", stream_name);
                    let node = Node::new(self.engine.nodes.len(), node_name, NodeKind::Emit { sink: sink_kind });
                    let id   = self.engine.add_node(node);
                    self.engine.connect(upstream, id);
                }
            }
        }
    }

    fn type_check(&mut self) {
        if !self.engine.nodes.iter().any(|n| matches!(n.kind, NodeKind::Source { .. })) {
            self.warn("program declares no `source` – no events will flow");
        }
        if !self.engine.nodes.iter().any(|n| matches!(n.kind, NodeKind::Emit { .. })) {
            self.warn("program has no `emit` block – events are silently discarded");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// Compile a [`Program`] into a ready-to-run [`Engine`].
pub fn compile(program: &Program) -> CompileResult {
    let mut c = Compiler::new();
    for stmt in &program.stmts { c.compile_stmt(stmt); }
    c.type_check();
    let has_errors = c.diags.iter().any(|d| d.kind == ErrorKind::Error);
    if has_errors { Err(c.diags) } else { Ok((c.engine, c.diags)) }
}
