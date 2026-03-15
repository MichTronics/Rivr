//! # RIVR Runtime Values (v2 – embedded-hardened)
//!
//! ## Feature gates
//! | feature | `Str` type        | `Bytes` type       | `Window` available |
//! |---------|-------------------|--------------------|--------------------|
//! | `alloc` | `String`          | `Vec<u8>`          | yes                |
//!
//! The `alloc` feature is automatically enabled by `std`, so host builds are
//! unaffected.

// Import alloc types when no_std + alloc.  Under std these are in scope already.
#[cfg(all(feature = "alloc", not(feature = "std")))]
use alloc::{
    format,
    string::String,
    vec::Vec,
};

use serde::{Deserialize, Serialize};

#[cfg(not(feature = "alloc"))]
use super::fixed::FixedBytes;
use super::fixed::FixedText;

// ─────────────────────────────────────────────────────────────────────────────
// Storage type aliases  (change with feature flags)
// ─────────────────────────────────────────────────────────────────────────────

/// Heap string when `alloc` is enabled; 64-byte stack string otherwise.
#[cfg(feature = "alloc")]
pub type StrBuf = String;
#[cfg(not(feature = "alloc"))]
pub type StrBuf = FixedText<64>;

/// Heap byte buffer when `alloc` is enabled; 64-byte stack buffer otherwise.
#[cfg(feature = "alloc")]
pub type ByteBuf = Vec<u8>;
#[cfg(not(feature = "alloc"))]
pub type ByteBuf = FixedBytes<64>;

// ─────────────────────────────────────────────────────────────────────────────
// Value enum
// ─────────────────────────────────────────────────────────────────────────────

/// A concrete payload carried by every [`super::event::Event`].
///
/// Without the `alloc` feature the `Window` variant is omitted (windows
/// require heap allocation to store variable-length snapshots).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", content = "v")]
pub enum Value {
    Int(i64),
    Bool(bool),
    Str(StrBuf),
    Bytes(ByteBuf),
    Unit,
    /// Flushed window snapshot – list of string-encoded event values.
    /// **Only available with the `alloc` feature.**
    #[cfg(feature = "alloc")]
    Window(Vec<String>),
}

// ─────────────────────────────────────────────────────────────────────────────
// Methods
// ─────────────────────────────────────────────────────────────────────────────

impl Value {
    #[allow(dead_code)]
    pub fn as_int(&self) -> Option<i64> {
        if let Value::Int(n) = self {
            Some(*n)
        } else {
            None
        }
    }

    #[allow(dead_code)]
    pub fn as_str(&self) -> Option<&str> {
        if let Value::Str(s) = self {
            Some(s.as_ref())
        } else {
            None
        }
    }

    /// Return the **kind tag**: the first colon-delimited segment of a `Str`
    /// payload, or the variant name for non-string types.
    pub fn kind_tag(&self) -> &str {
        match self {
            Value::Str(s) => (s.as_ref() as &str).split(':').next().unwrap_or(""),
            Value::Int(_) => "Int",
            Value::Bool(_) => "Bool",
            Value::Bytes(_) => "Bytes",
            Value::Unit => "Unit",
            #[cfg(feature = "alloc")]
            Value::Window(_) => "Window",
        }
    }

    /// Return a human-readable string (alloc only).
    #[cfg(feature = "alloc")]
    pub fn display(&self) -> String {
        match self {
            Value::Int(n) => format!("{n}"),
            Value::Bool(b) => format!("{b}"),
            Value::Str(s) => s.clone(),
            Value::Bytes(b) => format!("<bytes len={}>", b.len()),
            Value::Unit => String::from("()"),
            Value::Window(w) => format!("[window {} events]", w.len()),
        }
    }

    /// Return a fixed-capacity display string (available in all build modes).
    pub fn display_fixed(&self) -> FixedText<256> {
        use core::fmt::Write as _;
        let mut t = FixedText::<256>::new();
        match self {
            Value::Int(n) => {
                t.push_i64(*n);
            }
            Value::Bool(b) => {
                let _ = write!(t, "{b}");
            }
            Value::Str(s) => {
                t.push_str(s.as_ref());
            }
            Value::Bytes(b) => {
                let _ = write!(t, "<bytes len={}>", b.len());
            }
            Value::Unit => {
                t.push_str("()");
            }
            #[cfg(feature = "alloc")]
            Value::Window(w) => {
                let _ = write!(t, "[window {} events]", w.len());
            }
        }
        t
    }
}

impl core::fmt::Display for Value {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        #[cfg(feature = "alloc")]
        {
            write!(f, "{}", self.display())
        }
        #[cfg(not(feature = "alloc"))]
        {
            write!(f, "{}", self.display_fixed())
        }
    }
}
