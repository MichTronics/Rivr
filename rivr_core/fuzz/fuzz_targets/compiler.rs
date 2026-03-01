//! fuzz_target: compiler
//!
//! Feeds successful parse results into the RIVR graph compiler.
//! The compiler **must never panic** on any syntactically valid program;
//! semantic errors must be returned as `CompileError` values.
//!
//! Strategy: parse first (skip if parse fails), then compile.  This focuses
//! the fuzzer on the compiler's graph-building and type-checking logic rather
//! than the parser's input validation.
//!
//! Run with:
//!   cargo fuzz run compiler
//!
//! Corpus shares seeds with dsl_parse.  Symlink or copy:
//!   ln -s ../dsl_parse/* fuzz/corpus/compiler/ 2>/dev/null || true
#![no_main]
use libfuzzer_sys::fuzz_target;
use rivr_core::{compiler, parser};

fuzz_target!(|data: &[u8]| {
    let Ok(src) = std::str::from_utf8(data) else { return };

    // Only continue with parseable programs.
    let Ok(program) = parser::parse(src) else { return };

    // The compiler MUST NOT panic on any valid AST.
    // Compile errors (type mismatches, unresolved refs, etc.) are fine.
    let _ = compiler::compile(&program);
});
