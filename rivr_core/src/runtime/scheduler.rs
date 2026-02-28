//! # RIVR Scheduler (v2.1) – deterministic + clock-fair ordering
//!
//! ## Ordering within a single clock
//! Events for the same clock are dispatched strictly in `(tick, seq, node_id)`
//! order: earlier ticks first, injection order within the same tick.
//!
//! ## Cross-clock fairness
//! Without any fairness measure, clock 0 (mono) can starve clock 1 (lmp) if
//! it continuously produces events at small ticks.  The scheduler prevents
//! this by partitioning the work queue **per clock** and applying a
//! **weighted round-robin** on `pop()`:
//!
//! 1. Build the set of non-empty clock queues.
//! 2. Pop from the queue whose owning clock is next in the round-robin ring.
//! 3. Advance the ring pointer.
//!
//! Within each per-clock queue the ordering key `(tick, seq, node_id)` is
//! preserved, so all determinism guarantees remain intact.  The only change
//! is that *across* clocks we interleave rather than drain the globally
//! earliest clock first.
//!
//! ## Starvation detection
//! `clock_pending_counts() -> [usize; 8]` exposes per-clock queue depths so
//! callers can detect imbalance and emit diagnostics.

#[cfg(all(not(feature = "std"), not(feature = "alloc")))]
compile_error!("rivr_core::runtime::scheduler requires either 'std' or 'alloc' feature");

#[cfg(all(not(feature = "std"), feature = "alloc"))]
use alloc::collections::{BTreeMap, BinaryHeap};
#[cfg(feature = "std")]
use std::collections::{BTreeMap, BinaryHeap};

use core::cmp::Reverse;

use super::event::Event;
use super::node::NodeId;

// ─────────────────────────────────────────────────────────────────────────────
// WorkItem & ordering
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct WorkItem {
    pub node_id: NodeId,
    pub event: Event,
}

/// Per-clock ordering key (clock is implicit from the queue bucket).
#[derive(PartialEq, Eq, PartialOrd, Ord)]
struct IntraKey {
    tick: u64,
    seq: u32,
    node_id: usize,
}

impl IntraKey {
    fn from(w: &WorkItem) -> Self {
        Self {
            tick: w.event.stamp.tick,
            seq: w.event.seq,
            node_id: w.node_id,
        }
    }
}

struct Entry {
    key: Reverse<IntraKey>,
    item: WorkItem,
}

impl PartialEq for Entry {
    fn eq(&self, o: &Self) -> bool {
        self.key == o.key
    }
}
impl Eq for Entry {}
impl PartialOrd for Entry {
    fn partial_cmp(&self, o: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(o))
    }
}
impl Ord for Entry {
    fn cmp(&self, o: &Self) -> core::cmp::Ordering {
        self.key.cmp(&o.key)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Scheduler
// ─────────────────────────────────────────────────────────────────────────────

/// Priority work queue with **per-clock fairness**.
///
/// Each clock domain gets its own priority sub-queue.  On `pop()` we cycle
/// through active clocks in round-robin order, picking the earliest event
/// from the selected clock''s queue.
pub struct Scheduler {
    /// Per-clock priority queues.  Keys are clock ids (u8).
    queues: BTreeMap<u8, BinaryHeap<Entry>>,
    /// Ordered list of active (non-empty) clock ids, rebuilt lazily.
    clock_order: [u8; 8],
    /// Number of distinct clocks with pending items.
    active_clocks: usize,
    /// Round-robin position (index into `clock_order[..active_clocks]`).
    rr_pos: usize,

    pub total_enqueued: u64,
    pub total_processed: u64,
}

impl Scheduler {
    pub fn new() -> Self {
        Self {
            queues: BTreeMap::new(),
            clock_order: [0; 8],
            active_clocks: 0,
            rr_pos: 0,
            total_enqueued: 0,
            total_processed: 0,
        }
    }

    // ── push ──────────────────────────────────────────────────────────────

    pub fn push(&mut self, node_id: NodeId, event: Event) {
        self.total_enqueued += 1;
        let clk = event.stamp.clock;
        let item = WorkItem { node_id, event };
        let key = Reverse(IntraKey::from(&item));
        self.queues
            .entry(clk)
            .or_default()
            .push(Entry { key, item });
        self.rebuild_order();
    }

    // ── pop (fair round-robin across clocks) ─────────────────────────────

    pub fn pop(&mut self) -> Option<WorkItem> {
        if self.active_clocks == 0 {
            return None;
        }

        // Advance round-robin until we find a non-empty queue.
        let n = self.active_clocks;
        for _ in 0..n {
            let clk = self.clock_order[self.rr_pos % n];
            self.rr_pos = (self.rr_pos + 1) % n;
            if let Some(q) = self.queues.get_mut(&clk) {
                if let Some(entry) = q.pop() {
                    self.total_processed += 1;
                    if q.is_empty() {
                        self.rebuild_order();
                    }
                    return Some(entry.item);
                }
            }
        }
        None
    }

    // ── diagnostics ───────────────────────────────────────────────────────

    pub fn is_idle(&self) -> bool {
        self.active_clocks == 0
    }

    #[allow(dead_code)]
    pub fn depth(&self) -> usize {
        self.queues.values().map(|q| q.len()).sum()
    }

    /// Return per-clock pending event counts (indexed by clock id 0..7).
    pub fn clock_pending_counts(&self) -> [usize; 8] {
        let mut counts = [0usize; 8];
        for (&clk, q) in &self.queues {
            if clk < 8 {
                counts[clk as usize] = q.len();
            }
        }
        counts
    }

    // ── internal ──────────────────────────────────────────────────────────

    fn rebuild_order(&mut self) {
        let mut n = 0usize;
        for (&clk, q) in &self.queues {
            if !q.is_empty() && n < 8 {
                self.clock_order[n] = clk;
                n += 1;
            }
        }
        self.active_clocks = n;
        // Keep rr_pos in range after a potential shrink
        if n > 0 {
            self.rr_pos %= n;
        } else {
            self.rr_pos = 0;
        }
    }
}

impl Default for Scheduler {
    fn default() -> Self {
        Self::new()
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Unit tests
// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::runtime::{Event, Value};

    fn ev(ms: u64) -> Event {
        Event::mono(ms, Value::Int(ms as i64))
    }

    /// A fresh scheduler is idle and has zero depth.
    #[test]
    fn new_scheduler_is_idle() {
        let s = Scheduler::new();
        assert!(s.is_idle());
        assert_eq!(s.depth(), 0);
    }

    /// Pushing an event makes the scheduler non-idle; popping it restores idle.
    #[test]
    fn push_pop_round_trip() {
        let mut s = Scheduler::new();
        s.push(0, ev(100));
        assert!(!s.is_idle());
        assert_eq!(s.depth(), 1);

        let item = s.pop().expect("pop must return the pushed item");
        assert_eq!(item.node_id, 0);
        assert!(s.is_idle());
        assert_eq!(s.depth(), 0);
    }

    /// Events on the same clock come out in tick order.
    #[test]
    fn same_clock_tick_order() {
        let mut s = Scheduler::new();
        s.push(0, ev(300));
        s.push(0, ev(100));
        s.push(0, ev(200));

        let t1 = s.pop().unwrap().event.stamp.tick;
        let t2 = s.pop().unwrap().event.stamp.tick;
        let t3 = s.pop().unwrap().event.stamp.tick;
        assert!(t1 <= t2 && t2 <= t3, "events must emerge in tick order");
    }

    /// Two clocks must interleave (neither starves the other) over
    /// an alternating push/pop sequence.
    #[test]
    fn two_clocks_interleave() {
        let mut s = Scheduler::new();
        // Push 3 events on clock 0 and 3 on clock 1.
        for i in 0..3u64 {
            let mut e0 = ev(i);
            e0.stamp.clock = 0;
            s.push(0, e0);
            let mut e1 = ev(i);
            e1.stamp.clock = 1;
            s.push(1, e1);
        }

        let mut clocks_seen = [0u32; 2];
        while let Some(item) = s.pop() {
            let clk = item.event.stamp.clock as usize;
            if clk < 2 {
                clocks_seen[clk] += 1;
            }
        }
        assert_eq!(clocks_seen[0], 3, "clock 0 must yield all 3 events");
        assert_eq!(clocks_seen[1], 3, "clock 1 must yield all 3 events");
    }

    /// `clock_pending_counts` accurately reflects per-clock depth.
    #[test]
    fn clock_pending_counts() {
        let mut s = Scheduler::new();
        let mut e = ev(1);
        e.stamp.clock = 3;
        s.push(0, e);

        let counts = s.clock_pending_counts();
        assert_eq!(counts[3], 1, "clock 3 must show 1 pending event");
        for i in [0usize, 1, 2, 4, 5, 6, 7] {
            assert_eq!(counts[i], 0);
        }
    }

    /// Popping from an empty scheduler must not panic.
    #[test]
    fn pop_empty_is_none() {
        let mut s = Scheduler::new();
        assert!(s.pop().is_none());
    }
}
