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
//! full `std`.
//!
//! ## Feature flags
//! | feature | effect                                                  |
//! |---------|----------------------------------------------------------|
//! | `std`   | enables std + alloc + serde_json (default host build)   |
//! | `alloc` | enables heap types without std (e.g. cortex-m + alloc)  |
//! | `ffi`   | enables the C ABI and implies `alloc`                    |

#![cfg_attr(not(feature = "std"), no_std)]

#[cfg(not(feature = "std"))]
extern crate alloc;

#[cfg(all(not(feature = "std"), not(feature = "alloc")))]
compile_error!("rivr_core currently requires either the `std` or `alloc` feature");

// Sub-systems
pub mod adapt;
pub mod ast;
pub mod compiler;
pub mod parser;
pub mod runtime;

/// C-ABI exports for embedding rivr_core in C firmware (ESP32, etc.).
/// Compiled in when the `ffi` feature is enabled or when building a staticlib.
#[cfg(feature = "ffi")]
pub mod ffi;

// Convenience top-level re-exports.
pub use compiler::{compile, CompileError};
pub use parser::{parse, ParseError};
pub use runtime::{
    ByteBuf, EffectRecord, Engine, Event, OpCode, SourceMetrics, Stamp, StrBuf, TraceRecord, Value,
};

#[cfg(all(not(feature = "std"), not(test)))]
mod embedded_rt {
    use core::alloc::{GlobalAlloc, Layout};
    use core::panic::PanicInfo;

    struct LibcAlloc;

    unsafe impl GlobalAlloc for LibcAlloc {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            unsafe extern "C" {
                fn malloc(size: usize) -> *mut u8;
            }
            unsafe { malloc(layout.size()) }
        }

        unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
            unsafe extern "C" {
                fn free(ptr: *mut u8);
            }
            unsafe { free(ptr) }
        }

        unsafe fn realloc(&self, ptr: *mut u8, _layout: Layout, new_size: usize) -> *mut u8 {
            unsafe extern "C" {
                fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
            }
            unsafe { realloc(ptr, new_size) }
        }
    }

    #[global_allocator]
    static ALLOC: LibcAlloc = LibcAlloc;

    #[panic_handler]
    fn panic(_: &PanicInfo<'_>) -> ! {
        unsafe extern "C" {
            fn abort() -> !;
        }
        unsafe { abort() }
    }
}
