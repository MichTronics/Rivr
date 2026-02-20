//! # RIVR Replay Log
//!
//! For deterministic replay, RIVR records every event injected into a *source*
//! node to a JSONL (newline-delimited JSON) file.  During replay mode, the
//! engine reads events from this log rather than from live hardware sources.
//!
//! ## File format
//! Each line is a JSON object:
//! ```json
//! {"source":"usb","t_ms":100,"v":{"type":"Str","v":"hello"}}
//! ```
//!
//! The log is append-only and must only be written during forward execution.
//! Reading the log is strictly sequential – line N is always processed before
//! line N+1 – which guarantees the same deterministic order.

use std::io::{BufRead, BufReader, Write};

use serde::{Deserialize, Serialize};

use super::event::Event;
use super::value::Value;

// ─────────────────────────────────────────────────────────────────────────────

/// A single line in the JSONL replay log.
#[derive(Debug, Serialize, Deserialize)]
pub struct LogEntry {
    /// Name of the source node this event was injected into.
    pub source: String,
    /// Logical timestamp (milliseconds).
    pub t_ms: u64,
    /// The value carried by the event.
    pub v: Value,
}

// ─────────────────────────────────────────────────────────────────────────────

/// Append-only event log for deterministic replay.
///
/// # Usage
/// ```ignore
/// let mut log = ReplayLog::open_write("events.jsonl")?;
/// log.record("usb", &event)?;
/// ```
pub struct ReplayLog {
    writer: Box<dyn Write>,
}

impl ReplayLog {
    /// Open (or create and truncate) a JSONL file for writing.
    ///
    /// Each forward execution call overwrites the previous log so that the
    /// file always contains exactly the events from the most recent run.
    pub fn open_write(path: &str) -> std::io::Result<Self> {
        let file = std::fs::OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)   // start fresh each run – deterministic replay
            .open(path)?;
        Ok(Self { writer: Box::new(file) })
    }

    /// Write to an in-memory buffer (useful for tests).
    #[allow(dead_code)]
    pub fn open_memory(buf: Vec<u8>) -> Self {
        Self { writer: Box::new(std::io::Cursor::new(buf)) }
    }

    /// Record a source event.
    pub fn record(&mut self, source: &str, event: &Event) -> std::io::Result<()> {
        let entry = LogEntry {
            source: source.to_string(),
            t_ms:   event.t_ms,
            v:      event.v.clone(),
        };
        let line = serde_json::to_string(&entry)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))?;
        writeln!(self.writer, "{line}")?;
        Ok(())
    }
}

// ─────────────────────────────────────────────────────────────────────────────

/// An iterator over source events read from a JSONL replay file.
pub struct ReplayReader {
    lines: std::io::Lines<BufReader<std::fs::File>>,
}

impl ReplayReader {
    /// Open an existing JSONL replay file for reading.
    pub fn open(path: &str) -> std::io::Result<Self> {
        let file = std::fs::File::open(path)?;
        Ok(Self { lines: BufReader::new(file).lines() })
    }
}

impl Iterator for ReplayReader {
    type Item = std::io::Result<LogEntry>;

    fn next(&mut self) -> Option<Self::Item> {
        let line = self.lines.next()?.ok()?;
        if line.trim().is_empty() {
            return self.next(); // skip blank lines
        }
        Some(
            serde_json::from_str::<LogEntry>(&line)
                .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e)),
        )
    }
}

// ─────────────────────────────────────────────────────────────────────────────

/// Build a sequence of [`LogEntry`] objects from a JSONL string (for tests).
#[allow(dead_code)]
pub fn parse_jsonl(jsonl: &str) -> Vec<LogEntry> {
    jsonl
        .lines()
        .filter(|l| !l.trim().is_empty())
        .filter_map(|l| serde_json::from_str(l).ok())
        .collect()
}
