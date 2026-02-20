//! # rivr_core
//!
//! `no_std + alloc` library crate that contains:
//! - The RIVR AST
//! - The recursive-descent parser
//! - The graph compiler (AST → Engine DAG)
//! - The entire runtime (Stamp, Event, Node, Engine, Scheduler)
//! - ESP32-ready source adapters (ringbuffer, ISR-safe)
//!
//! ## no_std policy
//! This crate is `#![no_std]` by default.  The `"std"` feature (enabled by
//! default for host builds) re-enables the standard library.  The `"alloc"`
//! feature gives access to heap types (`Vec`, `String`, `BTreeMap`) without
//! full `std`.  Without both features, value payloads use `FixedText<64>` and
//! `FixedBytes<64>` from `runtime::fixed`.
//!
//! ## Feature flags
//! | feature | effect                                                  |
//! |---------|----------------------------------------------------------|
//! | `std`   | enables std + alloc + serde_json (default host build)   |
//! | `alloc` | enables heap types without std (e.g. cortex-m + alloc)  |
//! | (none)  | pure no-heap – uses FixedText / FixedBytes               |

#![cfg_attr(not(feature = "std"), no_std)]

#[cfg(not(feature = "std"))]
extern crate alloc;

// Sub-systems
pub mod ast;
pub mod compiler;
pub mod parser;
pub mod runtime;
pub mod adapt;

/// C-ABI exports for embedding rivr_core in C firmware (ESP32, etc.).
/// Compiled in when the `ffi` feature is enabled or when building a staticlib.
#[cfg(feature = "ffi")]
pub mod ffi;

// Convenience top-level re-exports.
pub use compiler::{compile, CompileError};
pub use parser::{parse, ParseError};
pub use runtime::{ByteBuf, Engine, Event, OpCode, Stamp, StrBuf, Value};
