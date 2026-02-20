//! # RIVR Scheduler
//!
//! The scheduler maintains the work queue that the engine drains during each
//! execution step.  It pairs an event with the target node that should receive
//! it, ensuring strictly FIFO delivery.
//!
//! **Determinism guarantee**: because the scheduler is a plain FIFO queue, the
//! execution order is fully determined by the order events were injected.
//! Given the same sequence of source events you always get the same sequence
//! of outputs – crucial for replay correctness.

use std::collections::VecDeque;

use super::node::NodeId;
use super::event::Event;

// ─────────────────────────────────────────────────────────────────────────────

/// A work item: a pending `(node_id, event)` pair waiting to be processed.
#[derive(Debug, Clone)]
pub struct WorkItem {
    pub node_id: NodeId,
    pub event:   Event,
}

// ─────────────────────────────────────────────────────────────────────────────

/// Single-threaded FIFO work queue.
///
/// The engine calls [`push`] to enqueue newly produced events and [`pop`]
/// to retrieve the next work item to process.
#[derive(Debug, Default)]
pub struct Scheduler {
    queue: VecDeque<WorkItem>,
    /// Total events enqueued over the lifetime of this scheduler (for stats).
    pub total_enqueued: u64,
    /// Total events processed over the lifetime of this scheduler.
    pub total_processed: u64,
}

impl Scheduler {
    /// Create a new, empty scheduler.
    pub fn new() -> Self {
        Self::default()
    }

    /// Enqueue `event` for delivery to `node_id`.
    pub fn push(&mut self, node_id: NodeId, event: Event) {
        self.total_enqueued += 1;
        self.queue.push_back(WorkItem { node_id, event });
    }

    /// Dequeue and return the next work item, or `None` if idle.
    pub fn pop(&mut self) -> Option<WorkItem> {
        let item = self.queue.pop_front();
        if item.is_some() {
            self.total_processed += 1;
        }
        item
    }

    /// Return `true` if there is nothing left to process.
    pub fn is_idle(&self) -> bool {
        self.queue.is_empty()
    }

    /// Current depth of the work queue.
    #[allow(dead_code)]
    pub fn depth(&self) -> usize {
        self.queue.len()
    }
}
