//! # rivr_core runtime
//!
//! Sub-modules of the RIVR stream-graph runtime.

pub mod bounded;
pub mod engine;
pub mod event;
pub mod fixed;
pub mod node;
pub mod opcode;
pub mod scheduler;
pub mod value;

#[cfg(feature = "ffi")]
pub mod alloc_guard;

// Top-level re-exports.
pub use engine::{EffectRecord, Engine, SourceMetrics, TraceRecord};
pub use event::{Event, Stamp};
pub use node::{DropPolicy, Node, NodeId, NodeKind, SinkKind, QUEUE_CAP, WINDOW_CAP};
pub use opcode::OpCode;
pub use scheduler::Scheduler;
pub use value::{ByteBuf, StrBuf, Value};
