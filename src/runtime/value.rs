//! # RIVR Runtime Values
//!
//! [`Value`] is the only data type that flows through the stream graph at
//! runtime.  Keeping the enum small is important for embedded targets where
//! both stack and heap are precious.

use serde::{Deserialize, Serialize};

// ─────────────────────────────────────────────────────────────────────────────

/// The set of concrete values that can be carried by a [`super::event::Event`].
///
/// All variants are cheap to clone – `Str` and `Bytes` hold heap-allocated
/// data but are only cloned when a node fans out to multiple outputs, so in
/// practice most streams move values without copying.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", content = "v")]
pub enum Value {
    /// 64-bit signed integer.
    Int(i64),
    /// Boolean flag.
    Bool(bool),
    /// UTF-8 text (most common in USB / debug streams).
    Str(String),
    /// Raw byte payload (LoRa frames, sensor data).
    Bytes(Vec<u8>),
    /// Represents "no value" / unit – emitted by sinks to signal completion.
    Unit,
    /// A flushed window – carries the collected events as a nested list.
    /// We encode the inner events as a JSON string to keep the enum flat and
    /// avoid recursion inside `Value`.
    Window(Vec<String>),
}

impl Value {
    /// Return the string content if this is `Value::Str`, or `None`.
    #[allow(dead_code)]
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// Return the integer content if this is `Value::Int`, or `None`.
    #[allow(dead_code)]
    pub fn as_int(&self) -> Option<i64> {
        match self {
            Value::Int(n) => Some(*n),
            _ => None,
        }
    }

    /// Human-readable display, used by the `io.usb.print` sink and trace logs.
    pub fn display(&self) -> String {
        match self {
            Value::Int(n)     => n.to_string(),
            Value::Bool(b)    => b.to_string(),
            Value::Str(s)     => s.clone(),
            Value::Bytes(b)   => format!("<bytes len={}>", b.len()),
            Value::Unit       => "()".to_string(),
            Value::Window(w)  => format!("[window {} events]", w.len()),
        }
    }
}

impl std::fmt::Display for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.display())
    }
}
