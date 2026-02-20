//! # RingBuf – ISR-safe single-producer source adapter
//!
//! A fixed-capacity SPSC (single-producer single-consumer) ring buffer
//! designed for no-heap embedded targets.
//!
//! ## Contract
//! - **Producer side** (ISR / hardware task): call [`RingBuf::try_push`].
//!   Single-producer only – do not call from multiple ISRs concurrently
//!   without external synchronisation (e.g. a cortex-m `free()` critical
//!   section).
//! - **Consumer side** (engine task): call [`RingBuf::pop`] or
//!   [`RingBuf::drain_into_engine`].  Must run in a different context from
//!   the producer (no re-entrancy).
//!
//! ## Memory layout
//! The buffer is a `[MaybeUninit<T>; CAP]` array stored inline – zero heap.
//! `head` and `tail` are `usize` indices that wrap at `CAP`.

use core::mem::MaybeUninit;

#[cfg(feature = "alloc")]
use crate::runtime::engine::Engine;
#[cfg(feature = "alloc")]
use crate::runtime::event::{Event, Stamp};
#[cfg(feature = "alloc")]
use crate::runtime::value::Value;

// ─────────────────────────────────────────────────────────────────────────────

/// Fixed-capacity SPSC ring buffer with no heap allocation.
///
/// `T` must be `Copy` (e.g. fixed-size frame types, byte arrays, or
/// [`FixedText<N>`](crate::runtime::fixed::FixedText)).
///
/// `CAP` must be a power of two for the modulo-free index wrap to be optimal
/// (non-powers also work; the ring simply uses `% CAP`).
pub struct RingBuf<T: Copy, const CAP: usize> {
    buf:         [MaybeUninit<T>; CAP],
    /// Write index (producer advances this).
    head:        usize,
    /// Read index (consumer advances this).
    tail:        usize,
    /// Total items dropped due to overflow (diagnostics).
    pub drops:   u64,
    /// Total items successfully enqueued.
    pub pushed:  u64,
}

impl<T: Copy, const CAP: usize> RingBuf<T, CAP> {
    /// Create an empty ring buffer.
    ///
    /// Suitable for `static` initialisation.
    pub const fn new() -> Self {
        Self {
            // SAFETY: `MaybeUninit` does not require initialisation.
            buf:    unsafe { MaybeUninit::uninit().assume_init() },
            head:   0,
            tail:   0,
            drops:  0,
            pushed: 0,
        }
    }

    /// Number of items currently available to the consumer.
    #[inline]
    pub fn len(&self) -> usize {
        self.head.wrapping_sub(self.tail) % (CAP + 1)
    }

    pub fn is_empty(&self) -> bool { self.len() == 0 }

    /// `true` when the buffer can hold no more items without overwriting.
    #[inline]
    pub fn is_full(&self) -> bool { self.len() >= CAP }

    /// **Producer**: push one item.  Returns `false` if the buffer is full
    /// and the item was dropped.
    pub fn try_push(&mut self, item: T) -> bool {
        if self.is_full() {
            self.drops += 1;
            return false;
        }
        let slot = self.head % CAP;
        self.buf[slot] = MaybeUninit::new(item);
        self.head = self.head.wrapping_add(1);
        self.pushed += 1;
        true
    }

    /// **Consumer**: pop one item.  Returns `None` if the buffer is empty.
    pub fn pop(&mut self) -> Option<T> {
        if self.is_empty() { return None; }
        let slot = self.tail % CAP;
        // SAFETY: slot is in [tail..head) which we have written.
        let item = unsafe { self.buf[slot].assume_init() };
        self.tail = self.tail.wrapping_add(1);
        Some(item)
    }

    /// **Consumer**: drain up to `limit` items, calling `f` for each one.
    pub fn drain_with<F: FnMut(T)>(&mut self, limit: usize, mut f: F) -> usize {
        let mut n = 0;
        while n < limit {
            match self.pop() {
                Some(item) => { f(item); n += 1; }
                None       => break,
            }
        }
        n
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Engine integration (requires alloc for Event / Engine)
// ─────────────────────────────────────────────────────────────────────────────

#[cfg(feature = "alloc")]
impl<const CAP: usize> RingBuf<[u8; 64], CAP> {
    /// Drain all buffered byte frames into the RIVR engine.
    ///
    /// Each frame is injected as a `Value::Bytes` event with the provided
    /// `stamp`.  The stamp's `tick` is *not* auto-incremented – supply the
    /// current logical tick from your RTOS task.
    ///
    /// Returns the number of events injected.
    pub fn drain_into_engine(
        &mut self,
        engine:      &mut Engine,
        source_name: &str,
        stamp:       Stamp,
    ) -> usize {
        let mut n = 0usize;
        while let Some(frame) = self.pop() {
            let ev = Event {
                stamp,
                v:   Value::Bytes(frame.to_vec()),
                tag: None,
                seq: 0,
            };
            let _ = engine.inject(source_name, ev);
            n += 1;
        }
        n
    }
}

impl<T: Copy, const CAP: usize> core::fmt::Debug for RingBuf<T, CAP> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "RingBuf {{ len: {}/{}, drops: {} }}", self.len(), CAP, self.drops)
    }
}
