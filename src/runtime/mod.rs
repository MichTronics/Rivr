//! # RIVR Runtime
//!
//! This module collects all runtime components needed to execute a compiled
//! RIVR program.  The public re-exports give callers (the compiler and main)
//! a single import path.

pub mod value;
pub mod event;
pub mod node;
pub mod engine;
pub mod scheduler;
pub mod replay;

// Convenience re-exports used throughout the codebase.
pub use value::Value;
pub use event::Event;
// Re-export full Node API for future embedded consumers.
#[allow(unused_imports)]
pub use node::{DropPolicy, Node, NodeId, NodeKind, SinkKind, QUEUE_CAPACITY};
pub use engine::Engine;
// Replay API – LogEntry is used by external tools (e.g. replay readers).
#[allow(unused_imports)]
pub use replay::{ReplayLog, ReplayReader, LogEntry};
