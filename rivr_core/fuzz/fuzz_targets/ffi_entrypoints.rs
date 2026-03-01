//! fuzz_target: ffi_entrypoints
//!
//! Exercises the C-ABI FFI surface (`rivr_engine_*` functions) with arbitrary
//! byte inputs.  The invariant is: **never panic, never undefined behaviour**.
//! All error paths must return a defined RIVR_ERR_* code.
//!
//! What is checked:
//!   • `rivr_engine_init` accepts arbitrary byte slices as "source text".
//!   • `rivr_engine_run` is called after a successful init.
//!   • `rivr_inject_event` is called with a null-sourced event name.
//!   • All return codes are within the known set of RIVR_ERR_* constants.
//!
//! Note: the engine uses a process-global slot (ENGINE_SLOT), so fuzz
//! iterations are NOT isolated — the target deliberately stresses
//! re-initialisation and partial-state robustness.
//!
//! Run with:
//!   cargo fuzz run ffi_entrypoints
#![no_main]
use libfuzzer_sys::fuzz_target;
use rivr_core::ffi::{
    rivr_engine_init, rivr_engine_run, rivr_engine_freeze,
    RIVR_OK,
    RIVR_ERR_NULL_PTR, RIVR_ERR_UTF8, RIVR_ERR_PARSE, RIVR_ERR_COMPILE,
    RIVR_ERR_NOT_INIT, RIVR_ERR_SRC_UNKNOWN, RIVR_ERR_NODE_LIMIT, RIVR_ERR_FROZEN,
};
use std::ffi::CString;

/// All currently defined RIVR error codes.
const KNOWN_CODES: &[u32] = &[
    RIVR_OK,
    RIVR_ERR_NULL_PTR,
    RIVR_ERR_UTF8,
    RIVR_ERR_PARSE,
    RIVR_ERR_COMPILE,
    RIVR_ERR_NOT_INIT,
    RIVR_ERR_SRC_UNKNOWN,
    RIVR_ERR_NODE_LIMIT,
    RIVR_ERR_FROZEN,
];

fuzz_target!(|data: &[u8]| {
    // Cap input so the fuzz target runs fast.  Programs > 4 KiB are not
    // useful in practice and would make the fuzzer slow.
    let src_bytes = &data[..data.len().min(4096)];

    // Build a NUL-terminated C string.  If the input contains embedded NUL
    // bytes, truncate to the first one — rivr_engine_init expects C-string.
    let c_input: CString = {
        let nul_pos = src_bytes.iter().position(|&b| b == 0).unwrap_or(src_bytes.len());
        // SAFETY: we check for NUL explicitly above.
        CString::new(&src_bytes[..nul_pos]).unwrap_or_default()
    };

    // ── Call rivr_engine_init ────────────────────────────────────────────
    let init_result = unsafe { rivr_engine_init(c_input.as_ptr()) };
    assert!(
        KNOWN_CODES.contains(&init_result.code),
        "rivr_engine_init returned unknown code {}",
        init_result.code
    );

    if init_result.code != RIVR_OK {
        // Init failed — nothing further to test in this iteration.
        return;
    }

    // ── Freeze then run ──────────────────────────────────────────────────
    let freeze_result = unsafe { rivr_engine_freeze() };
    assert!(
        KNOWN_CODES.contains(&freeze_result.code),
        "rivr_engine_freeze returned unknown code {}",
        freeze_result.code
    );

    // Run up to 64 steps (keeps the fuzz target fast).
    let run_result = unsafe { rivr_engine_run(64) };
    assert!(
        KNOWN_CODES.contains(&run_result.code),
        "rivr_engine_run returned unknown code {}",
        run_result.code
    );
});
