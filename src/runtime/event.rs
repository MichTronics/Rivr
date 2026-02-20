//! # RIVR Events
//!
//! An [`Event`] is the fundamental unit of data that flows through the stream
//! graph.  Every node receives `Event`s on its input queue and produces
//! `Event`s on its outputs.
//!
//! The `t_ms` timestamp is the *logical* time of the event (milliseconds since
//! the start of the simulation / replay).  The runtime never uses wall-clock
//! time directly; this keeps execution deterministic on any host.

use serde::{Deserialize, Serialize};

use super::value::Value;

// ─────────────────────────────────────────────────────────────────────────────

/// A timestamped value flowing through the RIVR stream graph.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Event {
    /// Logical timestamp in milliseconds.  Used by time-based operators such
    /// as `window.ms`, `throttle.ms`, and `debounce.ms`.
    pub t_ms: u64,

    /// The payload carried by this event.
    pub v: Value,

    /// Optional trace tag injected by the `tag("label")` operator.  `None`
    /// means this event has not been tagged.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tag: Option<String>,
}

impl Event {
    /// Create a plain (untagged) event.
    pub fn new(t_ms: u64, v: Value) -> Self {
        Self { t_ms, v, tag: None }
    }

    /// Clone this event with a new logical timestamp.
    #[allow(dead_code)]
    pub fn with_time(self, t_ms: u64) -> Self {
        Self { t_ms, ..self }
    }

    /// Clone this event attaching (or replacing) the trace tag.
    pub fn with_tag(self, tag: impl Into<String>) -> Self {
        Self { tag: Some(tag.into()), ..self }
    }
}

impl std::fmt::Display for Event {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if let Some(t) = &self.tag {
            write!(f, "[{}@{}ms tag={}]", self.v.display(), self.t_ms, t)
        } else {
            write!(f, "[{}@{}ms]", self.v.display(), self.t_ms)
        }
    }
}
