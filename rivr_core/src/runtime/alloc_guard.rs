//! Alloc freeze guard — called from C glue after `rivr_engine_init()` succeeds.
//!
//! Sets an atomic flag that marks the engine as "heap frozen".  Code that
//! performs alloc after this point can call [`alloc_is_frozen`] to assert
//! the invariant.
//!
//! A full enforcement (fault on every post-freeze `malloc`) would require a
//! custom `#[global_allocator]` wrapper; that is tracked as a future
//! hardening step.  For now the flag is used by debug `assert!` guards and
//! the Rust fuzz harness.

use core::sync::atomic::{AtomicBool, Ordering};

static ALLOC_FROZEN: AtomicBool = AtomicBool::new(false);

/// Returns `true` once [`rivr_runtime_freeze_alloc`] has been called.
#[inline]
pub fn alloc_is_frozen() -> bool {
    ALLOC_FROZEN.load(Ordering::Acquire)
}

#[cfg(feature = "ffi")]
#[no_mangle]
pub extern "C" fn rivr_runtime_freeze_alloc() {
    ALLOC_FROZEN.store(true, Ordering::Release);
}
