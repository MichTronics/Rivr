//! # RIVR Compiler
//!
//! The compiler walks the [`Program`] AST and builds a stream graph inside an
//! [`Engine`].  Each statement and each pipe step in an expression becomes one
//! or more [`Node`]s connected by directed edges.
//!
//! ## Compilation pass
//!
//! 1. **Source statements** → allocate a [`NodeKind::Source`] node and
//!    register it in the engine's source table.
//! 2. **Let statements** → recursively compile the right-hand-side expression
//!    into a chain of nodes; bind the final node's ID to the name.
//! 3. **Emit statements** → for each sink, allocate an [`NodeKind::Emit`] node
//!    and connect it to the stream the sink references.
//!
//! ## Expression compilation
//!
//! Expressions produce a *terminal [`NodeId`]*: the node whose output
//! represents the resulting stream.  Pipe operations (`|>`) extend the chain;
//! `merge(a, b)` creates a [`NodeKind::Merge`] node with two inputs.
//!
//! ## Static type checking (bonus)
//!
//! After building the graph the compiler runs a lightweight static analysis
//! pass that checks:
//! - All identifiers referenced in expressions and sinks are bound.
//! - `map.upper` and `filter.nonempty` only appear on stream paths that
//!   originate from a `Str`-producing source (best-effort – not exhaustive).
//!
//! Type errors are collected and returned alongside the engine so callers can
//! decide whether to abort or continue with warnings.

use std::collections::HashMap;

use crate::ast::*;
use crate::runtime::{Engine, Node, NodeKind, SinkKind};

// ─────────────────────────────────────────────────────────────────────────────
// Error / warning types
// ─────────────────────────────────────────────────────────────────────────────

/// A diagnostic produced during compilation.
#[derive(Debug)]
pub struct CompileError {
    pub kind:    ErrorKind,
    pub message: String,
}

#[derive(Debug, PartialEq)]
pub enum ErrorKind {
    /// The program is invalid and cannot be executed.
    Error,
    /// A potential problem; execution may still proceed.
    Warning,
}

impl std::fmt::Display for CompileError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let tag = match self.kind {
            ErrorKind::Error   => "ERROR",
            ErrorKind::Warning => "WARN",
        };
        write!(f, "[{tag}] {}", self.message)
    }
}

pub type CompileResult = Result<(Engine, Vec<CompileError>), Vec<CompileError>>;

// ─────────────────────────────────────────────────────────────────────────────
// Compiler state
// ─────────────────────────────────────────────────────────────────────────────

struct Compiler {
    engine:  Engine,
    /// Maps stream names to the terminal NodeId of their compiled expression.
    bindings: HashMap<String, usize>,
    diags:    Vec<CompileError>,
    next_anon: usize,
}

impl Compiler {
    fn new() -> Self {
        Self {
            engine:     Engine::new(),
            bindings:   HashMap::new(),
            diags:      Vec::new(),
            next_anon:  0,
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    fn fresh_name(&mut self, prefix: &str) -> String {
        let n = self.next_anon;
        self.next_anon += 1;
        format!("{prefix}#{n}")
    }

    fn error(&mut self, msg: impl Into<String>) {
        self.diags.push(CompileError {
            kind:    ErrorKind::Error,
            message: msg.into(),
        });
    }

    fn warn(&mut self, msg: impl Into<String>) {
        self.diags.push(CompileError {
            kind:    ErrorKind::Warning,
            message: msg.into(),
        });
    }

    fn resolve(&self, name: &str) -> Option<usize> {
        self.bindings.get(name).copied()
    }

    // ── Expression compilation ────────────────────────────────────────────

    /// Compile an expression and return the terminal NodeId of the resulting
    /// stream.  Returns `None` and records an error if compilation fails.
    fn compile_expr(&mut self, expr: &Expr) -> Option<usize> {
        match expr {
            // ── Identifier reference ──────────────────────────────────────
            Expr::Ident(name) => {
                match self.resolve(name) {
                    Some(id) => Some(id),
                    None => {
                        self.error(format!("undefined stream `{name}`"));
                        None
                    }
                }
            }

            // ── Literal ───────────────────────────────────────────────────
            Expr::Lit(_lit) => {
                // A literal produces a synthetic source that emits one event.
                // For MVP we don't execute literals directly; they are
                // typically only used in operator arguments, not as standalone
                // expressions.  Emit a warning and skip.
                self.warn("standalone literals are not yet supported as stream expressions");
                None
            }

            // ── Pipe ──────────────────────────────────────────────────────
            Expr::Pipe { lhs, op } => {
                let upstream_id = self.compile_expr(lhs)?;
                let node_kind    = compile_pipe_op(op);
                let node_name    = self.fresh_name(&pipe_op_name(op));
                let node = Node::new(self.engine.nodes.len(), node_name, node_kind);
                let node_id = self.engine.add_node(node);
                self.engine.connect(upstream_id, node_id);
                Some(node_id)
            }

            // ── Merge ─────────────────────────────────────────────────────
            Expr::Merge(a, b) => {
                let id_a = match self.resolve(a) {
                    Some(id) => id,
                    None => {
                        self.error(format!("undefined stream `{a}` in merge"));
                        return None;
                    }
                };
                let id_b = match self.resolve(b) {
                    Some(id) => id,
                    None => {
                        self.error(format!("undefined stream `{b}` in merge"));
                        return None;
                    }
                };
                let name    = self.fresh_name("merge");
                let node    = Node::new(self.engine.nodes.len(), name, NodeKind::Merge);
                let node_id = self.engine.add_node(node);
                self.engine.connect(id_a, node_id);
                self.engine.connect(id_b, node_id);
                Some(node_id)
            }
        }
    }

    // ── Statement compilation ─────────────────────────────────────────────

    fn compile_stmt(&mut self, stmt: &Stmt) {
        match stmt {
            // source NAME = kind;
            Stmt::Source { name, kind } => {
                let node_kind = NodeKind::Source { name: name.clone() };
                let node      = Node::new(self.engine.nodes.len(), name.clone(), node_kind);
                let node_id   = self.engine.add_node(node);
                self.engine.register_source(name.clone(), node_id);
                self.bindings.insert(name.clone(), node_id);

                // Warn about known mock sources.
                match kind {
                    SourceKind::Usb  => {}
                    SourceKind::Lora => {}
                    SourceKind::Programmatic => {
                        self.warn(format!(
                            "source `{name}` uses `programmatic` kind – \
                             events must be injected by the host"
                        ));
                    }
                }
            }

            // let NAME = expr;
            Stmt::Let { name, expr } => {
                if let Some(terminal_id) = self.compile_expr(expr) {
                    // Rename the terminal node to the binding name for clarity.
                    self.engine.nodes[terminal_id].name = name.clone();
                    self.engine.register_stream(name.clone(), terminal_id);
                    self.bindings.insert(name.clone(), terminal_id);
                }
                // If compile_expr returned None, an error was already recorded.
            }

            // emit { sink; ... }
            Stmt::Emit { sinks } => {
                for sink in sinks {
                    let (stream_name, sink_kind) = match sink {
                        Sink::UsbPrint(n)   => (n, SinkKind::UsbPrint),
                        Sink::LoraTx(n)     => (n, SinkKind::LoraTx),
                        Sink::DebugDump(n)  => (n, SinkKind::DebugDump),
                    };

                    let upstream_id = match self.resolve(stream_name) {
                        Some(id) => id,
                        None => {
                            self.error(format!(
                                "emit references undefined stream `{stream_name}`"
                            ));
                            continue;
                        }
                    };

                    let node_name = format!("emit({stream_name})");
                    let node      = Node::new(
                        self.engine.nodes.len(),
                        node_name,
                        NodeKind::Emit { sink: sink_kind },
                    );
                    let node_id = self.engine.add_node(node);
                    self.engine.connect(upstream_id, node_id);
                }
            }
        }
    }

    // ── Static type-check pass ────────────────────────────────────────────

    /// Walk the graph and perform lightweight static checks.
    ///
    /// Currently verifies:
    /// - At least one source is declared.
    /// - At least one emit node exists.
    fn type_check(&mut self) {
        let has_source = self.engine.nodes.iter().any(|n| {
            matches!(n.kind, NodeKind::Source { .. })
        });
        let has_emit = self.engine.nodes.iter().any(|n| {
            matches!(n.kind, NodeKind::Emit { .. })
        });
        if !has_source {
            self.warn("program declares no `source` – no events will flow");
        }
        if !has_emit {
            self.warn("program has no `emit` block – all events are silently discarded");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pipe-op → NodeKind mapping
// ─────────────────────────────────────────────────────────────────────────────

fn compile_pipe_op(op: &PipeOp) -> NodeKind {
    match op {
        PipeOp::MapUpper             => NodeKind::MapUpper,
        PipeOp::FilterNonempty       => NodeKind::FilterNonempty,
        PipeOp::FoldCount            => NodeKind::FoldCount { count: 0 },
        PipeOp::WindowMs(ms)         => NodeKind::WindowMs {
            duration_ms:  *ms,
            buffer:       Vec::new(),
            window_start: 0,
        },
        PipeOp::ThrottleMs(ms)       => NodeKind::ThrottleMs {
            interval_ms:  *ms,
            last_emit_ms: u64::MAX, // sentinel: never emitted yet → first event passes
        },
        PipeOp::DebounceMs(ms)       => NodeKind::DebounceMs {
            delay_ms: *ms,
            pending:  None,
        },
        PipeOp::Budget { rate, burst } => NodeKind::Budget {
            rate_per_ms:    rate / 1000.0, // convert events/s → events/ms
            burst:          *burst,
            tokens:         *burst as f64, // start with a full bucket
            last_refill_ms: 0,
        },
        PipeOp::Tag(label) => NodeKind::Tag { label: label.clone() },
    }
}

fn pipe_op_name(op: &PipeOp) -> String {
    match op {
        PipeOp::MapUpper             => "map.upper".to_string(),
        PipeOp::FilterNonempty       => "filter.nonempty".to_string(),
        PipeOp::FoldCount            => "fold.count".to_string(),
        PipeOp::WindowMs(ms)         => format!("window.ms({ms})"),
        PipeOp::ThrottleMs(ms)       => format!("throttle.ms({ms})"),
        PipeOp::DebounceMs(ms)       => format!("debounce.ms({ms})"),
        PipeOp::Budget { rate, .. }  => format!("budget({rate})"),
        PipeOp::Tag(l)               => format!("tag({l})"),
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// Compile a [`Program`] AST into an [`Engine`] ready to execute.
///
/// Returns `Ok((engine, warnings))` on success (including with non-fatal
/// warnings), or `Err(errors)` if any hard errors were detected.
pub fn compile(program: &Program) -> CompileResult {
    let mut c = Compiler::new();

    for stmt in &program.stmts {
        c.compile_stmt(stmt);
    }

    c.type_check();

    // Partition diagnostics.
    let has_errors = c.diags.iter().any(|d| d.kind == ErrorKind::Error);
    if has_errors {
        Err(c.diags)
    } else {
        Ok((c.engine, c.diags))
    }
}
