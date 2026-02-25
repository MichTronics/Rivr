//! # RIVR Abstract Syntax Tree (v2)
//!
//! Extended AST introducing:
//! - `Stamp`-aware clock annotations on sources (`source usb @mono = usb;`)
//! - Tick-domain operators (`window.ticks`, `delay.ticks`, `throttle.ticks`)
//! - `budget.airtime` – radio duty-cycle limiter (unique to RIVR)
//! - `filter.kind`    – payload-type discriminator for radio frames

#[cfg(not(feature = "std"))]
use alloc::{string::String, vec::Vec};

// ── Literals ────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
#[allow(dead_code)]
pub enum Literal {
    Int(i64),
    Float(f64),
    Str(String),
}

// ── Window capacity policy ────────────────────────────────────────────────

/// How a [`PipeOp::WindowTicksCapped`] handles a buffer that has reached
/// its `cap` limit *before* the tick-boundary fires.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowPolicy {
    /// Evict oldest event; then accept new event.  Classic sliding window.
    DropOldest,
    /// Discard new event; keep existing buffer.
    DropNewest,
    /// Flush the whole buffer immediately as a `Window` event; then accept
    /// the new event into a fresh buffer ("eager" flush).
    FlushEarly,
}

// ── Pipe operators ──────────────────────────────────────────────────────────

/// A single step in a `|>` pipeline.
#[derive(Debug, Clone, PartialEq)]
pub enum PipeOp {
    // ── Text / filter ───────────────────────────────────────────────────
    /// Uppercase all `Str` payloads.
    MapUpper,
    /// Drop events whose `Str` value is empty/whitespace.
    FilterNonempty,
    /// Drop events not matching the given kind tag.
    ///
    /// A "kind" is the first colon-delimited field of a string payload, e.g.
    /// `"CHAT:hello"` has kind `"CHAT"`.  Events that are not `Str` are also
    /// dropped unless the kind argument is `"*"`.
    FilterKind(String),

    // ── Aggregation ─────────────────────────────────────────────────────
    /// Running event counter; emits an `Int` on every event.
    FoldCount,

    // ── Time-domain (millisecond API – for wall-clock-aware host use) ────
    /// Tumbling window of `N` milliseconds  (host alias: maps to ticks).
    WindowMs(u64),
    /// Let at most one event through per N ms.
    ThrottleMs(u64),
    /// Wait N ms of silence before forwarding the last buffered event.
    DebounceMs(u64),

    // ── Tick-domain (canonical embedded API) ────────────────────────────
    /// Tumbling window that collects events for `N` clock ticks then flushes.
    WindowTicks(u64),
    /// Bounded tumbling window with an explicit capacity cap and overflow policy.
    ///
    /// `window.ticks(N, CAP, POLICY)` – if the buffer fills up before the
    /// `N`-tick boundary, apply `POLICY` (drop-oldest / drop-newest /
    /// flush-early).
    WindowTicksCapped {
        ticks:  u64,
        cap:    usize,
        policy: WindowPolicy,
    },
    /// Pass only `Value::Bytes` events whose byte at [`PKT_TYPE_BYTE_OFFSET`] (3)
    /// equals `pkt_type`.  Other value types are always dropped.
    ///
    /// Designed for the RIVR binary on-air packet format (Phase 4.1).
    /// Example: `filter.pkt_type(1)` → pass only `PKT_CHAT` frames.
    FilterPktType(u8),

    /// Forward at most one event per N ticks.
    ThrottleTicks(u64),
    /// Delay forwarding: emit buffered event after N ticks of silence.
    DelayTicks(u64),

    // ── Rate limiting ────────────────────────────────────────────────────
    /// Token-bucket limiter (generic): `rate` events/s, `burst` capacity.
    Budget { rate: f64, burst: u64 },

    /// **Airtime budget** – enforces a radio duty-cycle constraint.
    ///
    /// | param          | meaning                                        |
    /// |----------------|------------------------------------------------|
    /// | `window_ticks` | measurement window size in ticks               |
    /// | `duty`         | max fraction of the window that may be used    |
    ///
    /// Each forwarded event costs one "airtime unit".  When accumulated cost
    /// inside the current window exceeds `window_ticks * duty` the event is
    /// dropped.  The window resets automatically once the window expires.
    ///
    /// Example: `budget.airtime(360000, 0.10)` → max 36 000 ticks of airtime
    /// in every 360 000-tick window (≈ 10 % duty cycle on a 1 kHz mono clock).
    BudgetAirtime {
        window_ticks: u64,
        duty:         f64,
    },

    /// **Time-on-Air budget** – radio duty-cycle enforcer using actual airtime
    /// cost (microseconds) rather than event-count units.
    ///
    /// | param         | meaning                                            |
    /// |---------------|----------------------------------------------------|
    /// | `window_ms`   | measurement window in ticks (= ms on mono clock)   |
    /// | `duty`        | max fraction of the window used for TX             |
    /// | `toa_us`      | time-on-air cost per event in microseconds          |
    ///
    /// Example: `budget.toa_us(360000, 0.10, 400)` → max 144 s cumulative
    /// airtime in every 6-min window (each TX costs 400 μs).
    BudgetToaUs {
        window_ms: u64,
        duty:      f64,
        toa_us:    u64,
    },

    // ── Debug / tracing ─────────────────────────────────────────────────
    /// Attach a human-readable trace label to every passing event.
    Tag(String),

    // ── Merge (parsed as primary, not pipe) ─────────────────────────────
    // (kept here for completeness – see Expr::Merge)
}

// ── Expressions ─────────────────────────────────────────────────────────────

/// A RIVR expression – all expressions produce a *stream* at runtime.
#[derive(Debug, Clone, PartialEq)]
pub enum Expr {
    Ident(String),
    Lit(Literal),
    Pipe { lhs: Box<Expr>, op: PipeOp },
    Merge(String, String),
}

// ── Sink (inside emit blocks) ────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub enum Sink {
    UsbPrint(String),
    LoraTx(String),
    DebugDump(String),
}

// ── Source kind ───────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub enum SourceKind {
    Usb,
    Lora,
    Rf,
    Programmatic,
    /// Periodic timer source.  Fires an `Int(mono_ms)` event every
    /// `interval_ms` milliseconds on clock 0 (mono).
    /// Syntax: `source beacon = timer(30000);`
    Timer { interval_ms: u64 },
}

// ── Clock annotation ──────────────────────────────────────────────────────────

/// The optional `@clock_name` annotation on a `source` declaration.
///
/// During compilation this is resolved to a `clock: u8` id that is embedded
/// in every `Stamp` produced by that source.
///
/// Built-in clocks:
/// | name   | id | description                        |
/// |--------|----|------------------------------------|
/// | `mono` |  0 | Monotonic millisecond clock        |
/// | `lmp`  |  1 | LoRa mesh protocol logical clock   |
/// | `gps`  |  2 | GPS-derived absolute time          |
#[derive(Debug, Clone, PartialEq)]
pub struct ClockAnnotation {
    pub name: String,
}

// ── Statements ───────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub enum Stmt {
    /// `source NAME [@CLOCK] = <kind>;`
    Source {
        name:  String,
        clock: Option<ClockAnnotation>,
        kind:  SourceKind,
    },

    /// `let NAME = <expr>;`
    Let { name: String, expr: Expr },

    /// `emit { <sink>; ... }`
    Emit { sinks: Vec<Sink> },
}

// ── Program ───────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct Program {
    pub stmts: Vec<Stmt>,
}
