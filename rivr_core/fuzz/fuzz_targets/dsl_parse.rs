//! fuzz_target: dsl_parse
//!
//! Feeds arbitrary bytes (interpreted as UTF-8) into the RIVR DSL parser.
//! The parser **must never panic** — every error path must return a
//! `ParseError` rather than unwrapping/panicking.
//!
//! Run with:
//!   cargo fuzz run dsl_parse            # continuous fuzzing
//!   cargo fuzz run dsl_parse -- -runs=1000000   # bounded run
//!
//! Corpus seeds can be placed in `fuzz/corpus/dsl_parse/`.
#![no_main]
use libfuzzer_sys::fuzz_target;
use rivr_core::parser;

fuzz_target!(|data: &[u8]| {
    // Convert raw bytes to str; skip if not valid UTF-8 — the parser
    // only operates on &str.  Fuzzer will quickly learn valid prefixes.
    if let Ok(src) = std::str::from_utf8(data) {
        // The result is either Ok(program) or Err(parse_error).
        // Any panic here is a bug.
        let _ = parser::parse(src);
    }
});
