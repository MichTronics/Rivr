//! # RIVR OpCode Micro-VM
//!
//! `OpCode` is a compact, tag-only enumeration of every operator the RIVR
//! runtime can execute.  It forms the instruction set of the internal
//! micro-VM.
//!
//! ## Design intent
//! Today `Node` stores state in [`super::node::NodeKind`] – a rich tagged
//! union that carries both the opcode AND the mutable operator state in a
//! single enum.  That is ergonomic but makes it hard to:
//!
//! - Serialize just the *topology* of a graph without state.
//! - Optimise hot paths with a match on a small integer tag.
//! - Separate static (compile-time) and dynamic (runtime) data.
//!
//! `OpCode` is the first step toward a proper three-part `(OpCode, OpParams,
//! OpState)` split:
//!
//! ```text
//! OpCode   – what to execute     (one byte)
//! OpParams – immutable config    (set at compile time, never changes)
//! OpState  – mutable loop state  (changes each invocation)
//! ```
//!
//! The current sprint exposes `OpCode` and adds an `opcode()` introspection
//! method to [`super::node::NodeKind`].  The full split is deferred to
//! the next embedded sprint.
//!
//! ## Instruction set summary
//! | `OpCode`          | Operator            | Notes                        |
//! |-------------------|---------------------|------------------------------|
//! | `Source`          | `source NAME = …`   | pass-through injector        |
//! | `MapUpper`        | `map.upper()`       | ASCII uppercase              |
//! | `FilterNonempty`  | `filter.nonempty()` | drop blank strings           |
//! | `FilterKind`      | `filter.kind("K")`  | payload kind-tag filter      |
//! | `FoldCount`       | `fold.count()`      | monotonic counter            |
//! | `WindowTicks`     | `window.ticks(N)`   | tumbling window              |
//! | `ThrottleTicks`   | `throttle.ticks(N)` | rate limiter (tick-domain)   |
//! | `DelayTicks`      | `delay.ticks(N)`    | output delay                 |
//! | `WindowMs`        | `window.ms(N)`      | host alias → `WindowTicks`   |
//! | `ThrottleMs`      | `throttle.ms(N)`    | host alias → `ThrottleTicks` |
//! | `DebounceMs`      | `debounce.ms(N)`    | host alias → `DelayTicks`    |
//! | `Budget`          | `budget(r,b)`       | token-bucket limiter         |
//! | `AirtimeBudget`   | `budget.airtime(…)` | radio duty-cycle (by count)  |
//! | `ToaBudget`       | `budget.toa_us(…)`  | radio duty-cycle (by ToA)    |
//! | `Merge`           | `merge(a,b)`        | fan-in                       |
//! | `Tag`             | `tag("label")`      | event annotation             |
//! | `Emit`            | `emit { … }`        | sink / output                |

/// Compact opcode tag for every RIVR operator.
///
/// Each variant is a discriminant-only value (no data).  All runtime state
/// lives in the corresponding [`super::node::NodeKind`] variant until the
/// full state-split is implemented.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum OpCode {
    // ── Sources ─────────────────────────────────────────────────────────
    Source = 0x00,

    // ── Text / filter ────────────────────────────────────────────────────
    MapUpper = 0x10,
    /// map.lower() — ASCII lowercase
    MapLower = 0x14,
    /// map.trim()  — strip leading/trailing ASCII whitespace
    MapTrim = 0x15,
    FilterNonempty = 0x11,
    FilterKind = 0x12,
    /// filter.pkt_type(N) — inspect byte [3] of Value::Bytes (binary header)
    FilterPktType = 0x13,
    // ── Aggregation ──────────────────────────────────────────────────────
    FoldCount = 0x20,
    /// fold.sum() — running sum of Int payloads
    FoldSum = 0x21,
    /// fold.last() — emit the most recently seen value
    FoldLast = 0x22,

    // ── Tick-domain ──────────────────────────────────────────────────────
    WindowTicks = 0x30,
    ThrottleTicks = 0x31,
    DelayTicks = 0x32,

    // ── Ms-domain aliases ─────────────────────────────────────────────────
    WindowMs = 0x38,
    ThrottleMs = 0x39,
    DebounceMs = 0x3A,

    // ── Rate / duty-cycle ─────────────────────────────────────────────────
    Budget = 0x40,
    AirtimeBudget = 0x41,
    ToaBudget = 0x42,

    // ── Topology ─────────────────────────────────────────────────────────
    Merge = 0x50,
    Tag = 0x51,

    // ── Sinks ─────────────────────────────────────────────────────────────
    Emit = 0x60,
}

impl OpCode {
    /// Human-readable mnemonic used in disassembly / debug output.
    pub fn mnemonic(self) -> &'static str {
        match self {
            OpCode::Source => "source",
            OpCode::MapUpper => "map.upper",
            OpCode::MapLower => "map.lower",
            OpCode::MapTrim => "map.trim",
            OpCode::FilterNonempty => "filter.nonempty",
            OpCode::FilterKind => "filter.kind",
            OpCode::FilterPktType => "filter.pkt_type",
            OpCode::FoldCount => "fold.count",
            OpCode::FoldSum => "fold.sum",
            OpCode::FoldLast => "fold.last",
            OpCode::WindowTicks => "window.ticks",
            OpCode::ThrottleTicks => "throttle.ticks",
            OpCode::DelayTicks => "delay.ticks",
            OpCode::WindowMs => "window.ms",
            OpCode::ThrottleMs => "throttle.ms",
            OpCode::DebounceMs => "debounce.ms",
            OpCode::Budget => "budget",
            OpCode::AirtimeBudget => "budget.airtime",
            OpCode::ToaBudget => "budget.toa_us",
            OpCode::Merge => "merge",
            OpCode::Tag => "tag",
            OpCode::Emit => "emit",
        }
    }

    /// Hint: can this operator produce more than one output event per input?
    pub fn is_fan_out(self) -> bool {
        matches!(
            self,
            OpCode::WindowTicks | OpCode::WindowMs | OpCode::DebounceMs
        )
    }

    /// Is this a time-sensitive operator (has internal state that needs a tick)?
    pub fn is_time_sensitive(self) -> bool {
        matches!(
            self,
            OpCode::WindowTicks
                | OpCode::ThrottleTicks
                | OpCode::DelayTicks
                | OpCode::WindowMs
                | OpCode::ThrottleMs
                | OpCode::DebounceMs
                | OpCode::Budget
                | OpCode::AirtimeBudget
                | OpCode::ToaBudget
        )
    }
}

impl core::fmt::Display for OpCode {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.mnemonic())
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// opcode() introspection on NodeKind
// ─────────────────────────────────────────────────────────────────────────────

use super::node::NodeKind;

impl NodeKind {
    /// Return the [`OpCode`] for this node without consuming state.
    pub fn opcode(&self) -> OpCode {
        match self {
            NodeKind::Source { .. } => OpCode::Source,
            NodeKind::MapUpper => OpCode::MapUpper,
            NodeKind::MapLower => OpCode::MapLower,
            NodeKind::MapTrim => OpCode::MapTrim,
            NodeKind::FilterNonempty => OpCode::FilterNonempty,
            NodeKind::FilterKind { .. } => OpCode::FilterKind,
            NodeKind::FilterPktType { .. } => OpCode::FilterPktType,
            NodeKind::FoldCount { .. } => OpCode::FoldCount,
            NodeKind::FoldSum { .. } => OpCode::FoldSum,
            NodeKind::FoldLast { .. } => OpCode::FoldLast,
            NodeKind::WindowTicks { .. } => OpCode::WindowTicks,
            NodeKind::ThrottleTicks { .. } => OpCode::ThrottleTicks,
            NodeKind::DelayTicks { .. } => OpCode::DelayTicks,
            NodeKind::WindowMs { .. } => OpCode::WindowMs,
            NodeKind::ThrottleMs { .. } => OpCode::ThrottleMs,
            NodeKind::DebounceMs { .. } => OpCode::DebounceMs,
            NodeKind::Budget { .. } => OpCode::Budget,
            NodeKind::AirtimeBudget { .. } => OpCode::AirtimeBudget,
            NodeKind::ToaBudget { .. } => OpCode::ToaBudget,
            NodeKind::Merge => OpCode::Merge,
            NodeKind::Tag { .. } => OpCode::Tag,
            NodeKind::Emit { .. } => OpCode::Emit,
        }
    }
}
