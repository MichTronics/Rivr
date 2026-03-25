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
    use core::cmp;
    use core::panic::PanicInfo;
    use core::ptr;

    struct LibcAlloc;

    const MALLOC_MIN_ALIGN: usize = core::mem::size_of::<usize>();

    unsafe impl GlobalAlloc for LibcAlloc {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            unsafe extern "C" {
                fn malloc(size: usize) -> *mut u8;
            }

            if layout.size() == 0 {
                return layout.align() as *mut u8;
            }

            if layout.align() <= MALLOC_MIN_ALIGN {
                return unsafe { malloc(layout.size()) };
            }

            // Over-aligned allocation without posix_memalign (not available on all targets).
            // Allocate extra space, manually align the pointer, and store the original
            // malloc pointer in the word immediately before the aligned region so that
            // dealloc can recover it and pass the correct pointer to free().
            let store_size = core::mem::size_of::<usize>();
            let total = match layout.size().checked_add(layout.align() - 1 + store_size) {
                Some(n) => n,
                None => return ptr::null_mut(),
            };
            let raw = unsafe { malloc(total) };
            if raw.is_null() {
                return ptr::null_mut();
            }
            let aligned_addr = (raw as usize + store_size + layout.align() - 1) & !(layout.align() - 1);
            let aligned = aligned_addr as *mut u8;
            // Safety: aligned is always at least store_size bytes past raw.
            unsafe { ptr::write(aligned.sub(store_size) as *mut usize, raw as usize) };
            aligned
        }

        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            unsafe extern "C" {
                fn free(ptr: *mut u8);
            }

            if ptr.is_null() {
                return;
            }

            if layout.align() <= MALLOC_MIN_ALIGN {
                unsafe { free(ptr) };
            } else {
                // Recover the original malloc pointer stored before the aligned region.
                let store_size = core::mem::size_of::<usize>();
                let orig = unsafe { ptr::read(ptr.sub(store_size) as *const usize) } as *mut u8;
                unsafe { free(orig) };
            }
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            unsafe extern "C" {
                fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
            }

            if layout.size() == 0 {
                let new_layout = match Layout::from_size_align(new_size, layout.align()) {
                    Ok(l) => l,
                    Err(_) => return ptr::null_mut(),
                };
                return unsafe { self.alloc(new_layout) };
            }

            if new_size == 0 {
                unsafe { self.dealloc(ptr, layout) };
                return layout.align() as *mut u8;
            }

            if layout.align() <= MALLOC_MIN_ALIGN {
                return unsafe { realloc(ptr, new_size) };
            }

            // For over-aligned allocations, grow/shrink via alloc+copy+free.
            let new_layout = match Layout::from_size_align(new_size, layout.align()) {
                Ok(l) => l,
                Err(_) => return ptr::null_mut(),
            };
            let new_ptr = unsafe { self.alloc(new_layout) };
            if new_ptr.is_null() {
                return ptr::null_mut();
            }
            unsafe { ptr::copy_nonoverlapping(ptr, new_ptr, cmp::min(layout.size(), new_size)) };
            unsafe { self.dealloc(ptr, layout) };
            new_ptr
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
