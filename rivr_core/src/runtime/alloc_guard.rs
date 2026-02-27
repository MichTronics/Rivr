//! Alloc freeze guard — called from C glue after `rivr_engine_init()` succeeds.
//!
//! In a production hardened build this is the "heap is now frozen" marker.
//! The hook is intentionally empty today; a future malloc-wrapping allocator
//! can fault on all heap allocations after this point.

#[cfg(feature = "ffi")]
#[no_mangle]
pub extern "C" fn rivr_runtime_freeze_alloc() {
    // Future: switch allocator into read-only / panic-on-alloc mode.
}
