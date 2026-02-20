//! # RIVR Abstract Syntax Tree
//!
//! The AST is intentionally small.  Every RIVR program is a flat list of
//! statements; statements reference named streams; streams are composed by
//! piping through operators or merging two streams together.
//!
//! Grammar (informal):
//! ```
//! program  ::= stmt*
//! stmt     ::= "source" IDENT "=" source_kind ";"
//!            | "let"    IDENT "=" expr ";"
//!            | "emit"   "{" emit_stmt+ "}"
//! expr     ::= expr "|>" pipe_op
//!            | "merge" "(" IDENT "," IDENT ")"
//!            | IDENT
//!            | INT
//!            | STRING
//! pipe_op  ::= "map.upper" "()"
//!            | "filter.nonempty" "()"
//!            | "fold.count" "()"
//!            | "window.ms"   "(" INT ")"
//!            | "throttle.ms" "(" INT ")"
//!            | "debounce.ms" "(" INT ")"
//!            | "budget"      "(" FLOAT "," INT ")"
//!            | "tag"         "(" STRING ")"
//! sink     ::= "io.usb.print"  "(" IDENT ")"
//!            | "io.lora.tx"    "(" IDENT ")"
//!            | "io.debug.dump" "(" IDENT ")"
//! ```

// ── Literals ────────────────────────────────────────────────────────────────

/// Compile-time constant value that may appear in source text.
#[derive(Debug, Clone, PartialEq)]
#[allow(dead_code)]
pub enum Literal {
    Int(i64),
    Float(f64),
    Str(String),
}

// ── Pipe operators ──────────────────────────────────────────────────────────

/// A single step in a `|>` pipeline.
#[derive(Debug, Clone, PartialEq)]
pub enum PipeOp {
    /// Convert every `Str` event to its ASCII upper-case equivalent.
    MapUpper,
    /// Drop events whose `Str` value is empty after trimming.
    FilterNonempty,
    /// Accumulate a running event count; emits the new count on every event.
    FoldCount,
    /// Tumbling time window – collects events into a buffer and flushes the
    /// whole buffer as a single list event after `duration_ms` milliseconds.
    WindowMs(u64),
    /// Allow at most one event per `interval_ms` milliseconds; drop extras.
    ThrottleMs(u64),
    /// Restart a `delay_ms` timer on every event; only emit the *last* event
    /// once the timer fires without a new arrival (classic debounce).
    DebounceMs(u64),
    /// Token-bucket rate limiter.  `rate` tokens/second, `burst` maximum.
    Budget { rate: f64, burst: u64 },
    /// Attach a human-readable trace label to every event (for debug output).
    Tag(String),
}

// ── Expressions ─────────────────────────────────────────────────────────────

/// A RIVR expression.  All expressions produce *streams* at runtime.
#[derive(Debug, Clone, PartialEq)]
pub enum Expr {
    /// Reference to a previously declared named stream.
    Ident(String),
    /// Literal constant (produces a single synthetic event).
    Lit(Literal),
    /// `lhs |> op` – apply `op` to every element of `lhs`.
    Pipe { lhs: Box<Expr>, op: PipeOp },
    /// `merge(a, b)` – interleave two streams in arrival order.
    Merge(String, String),
}

// ── Sink (inside emit blocks) ────────────────────────────────────────────────

/// Output actions available inside `emit { }` blocks.
#[derive(Debug, Clone, PartialEq)]
pub enum Sink {
    /// Print each event to stdout (models a USB-serial console).
    UsbPrint(String),
    /// Transmit each event payload over LoRa (mocked in MVP).
    LoraTx(String),
    /// Dump rich debug info for the named stream to stderr.
    DebugDump(String),
}

// ── Statements ───────────────────────────────────────────────────────────────

/// The kind of hardware / mock source this `source` declaration refers to.
#[derive(Debug, Clone, PartialEq)]
pub enum SourceKind {
    /// Mock USB-serial source – generates synthetic text lines.
    Usb,
    /// Mock LoRa RX source – generates synthetic byte frames.
    Lora,
    /// A programmatic source driven from within the host (used for replays).
    Programmatic,
}

/// A top-level RIVR statement.
#[derive(Debug, Clone, PartialEq)]
pub enum Stmt {
    /// `source NAME = <kind>;`
    ///
    /// Declares a named event source.  Sources are the *only* way new events
    /// enter the graph; all other nodes are derived from them.
    Source { name: String, kind: SourceKind },

    /// `let NAME = <expr>;`
    ///
    /// Binds the stream produced by `<expr>` to `NAME` so it can be referenced
    /// later in other expressions or inside emit blocks.
    Let { name: String, expr: Expr },

    /// `emit { <sink>; ... }`
    ///
    /// Groups one or more sink actions.  Only `emit` blocks may perform I/O.
    Emit { sinks: Vec<Sink> },
}

// ── Top-level program ────────────────────────────────────────────────────────

/// A complete RIVR program is just an ordered list of statements.
#[derive(Debug, Clone)]
pub struct Program {
    pub stmts: Vec<Stmt>,
}
