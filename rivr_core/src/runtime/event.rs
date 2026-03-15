//! # RIVR Events (v2) – Stamp-based logical time
//!
//! ## Why `Stamp`?
//! In multi-transport embedded systems (USB + LoRa + GPS + IMU) each
//! subsystem advances at its own rate and resolution.  A plain `u64`
//! millisecond counter cannot express "tick 42 on the LoRa mesh protocol
//! clock" vs "millisecond 1 042 on the monotonic system clock".
//!
//! `Stamp { clock, tick }` makes the clock domain explicit:
//! - `clock: u8` – which clock produced this tick (0 = mono, 1 = lmp, …)
//! - `tick:  u64` – absolute tick value on that clock
//!
//! The scheduler orders events by `(clock, tick, seq, node_id)` which gives
//! fully deterministic execution even when events from different clock domains
//! are interleaved.
//!
//! ## Sequence number `seq`
//! A monotonically increasing counter assigned by the engine at injection
//! time.  It breaks ties where two events share an identical stamp (e.g. two
//! sources fire at the same tick) and ensures FIFO ordering within the same
//! clock-tick.

#[cfg(not(feature = "std"))]
use alloc::{
    format,
    string::String,
};

use super::value::Value;
use serde::{Deserialize, Serialize};

// ─────────────────────────────────────────────────────────────────────────────
// Stamp
// ─────────────────────────────────────────────────────────────────────────────

/// Logical timestamp on a named clock domain.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Stamp {
    /// Clock domain identifier (0 = mono/ms, 1 = lmp, 2 = gps, …).
    pub clock: u8,
    /// Tick count on that clock.
    pub tick: u64,
}

impl Stamp {
    /// Convenience constructor.
    pub fn new(clock: u8, tick: u64) -> Self {
        Self { clock, tick }
    }

    /// Shorthand: mono clock tick 0.
    pub const ZERO: Self = Self { clock: 0, tick: 0 };

    /// Mono clock (id 0) at the given millisecond.
    pub fn mono(ms: u64) -> Self {
        Self { clock: 0, tick: ms }
    }

    /// Named-clock tick.
    pub fn at(clock: u8, tick: u64) -> Self {
        Self { clock, tick }
    }
}

/// Natural ordering: first by `clock`, then by `tick`.
/// Used by the scheduler to pick the globally earliest event.
impl PartialOrd for Stamp {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Stamp {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.clock
            .cmp(&other.clock)
            .then(self.tick.cmp(&other.tick))
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Event
// ─────────────────────────────────────────────────────────────────────────────

/// A stamped value flowing through the RIVR stream graph.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Event {
    /// Logical timestamp (clock domain + tick).
    pub stamp: Stamp,
    /// Payload.
    pub v: Value,
    /// Optional trace tag (injected by `tag("label")` operator).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tag: Option<String>,
    /// Injection sequence number – set by the engine, not by user code.
    #[serde(skip)]
    pub seq: u32,
}

impl Event {
    /// Create a plain untagged event with sequence 0.
    pub fn new(stamp: Stamp, v: Value) -> Self {
        Self {
            stamp,
            v,
            tag: None,
            seq: 0,
        }
    }

    /// Convenience: mono clock event at `ms` milliseconds.
    pub fn mono(ms: u64, v: Value) -> Self {
        Self::new(Stamp::mono(ms), v)
    }

    /// Clone attaching (or replacing) the trace tag.
    pub fn with_tag(self, tag: impl Into<String>) -> Self {
        Self {
            tag: Some(tag.into()),
            ..self
        }
    }

    /// Clone on a new stamp.
    #[allow(dead_code)]
    pub fn with_stamp(self, stamp: Stamp) -> Self {
        Self { stamp, ..self }
    }

    /// The tick value of this event on its clock.
    #[inline]
    pub fn tick(&self) -> u64 {
        self.stamp.tick
    }

    /// The clock domain of this event.
    #[inline]
    pub fn clock(&self) -> u8 {
        self.stamp.clock
    }
}

impl core::fmt::Display for Event {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let tag_part = self
            .tag
            .as_deref()
            .map(|t| format!(" tag={t}"))
            .unwrap_or_default();
        write!(
            f,
            "[{}@clk{}:tick{}{tag_part}]",
            self.v,
            self.stamp.clock,
            self.stamp.tick
        )
    }
}
