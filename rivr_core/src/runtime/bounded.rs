//! # BoundedVec – fixed-capacity Vec abstraction
//!
//! A `Vec<T>` wrapper that enforces a maximum capacity at runtime.
//! On embedded targets without dynamic allocation, this can be swapped for a
//! `heapless::Vec<T, N>` with matching semantics.
//!
//! The const generic `CAP` specifies the compile-time capacity ceiling.
//! Attempting to push beyond this limit triggers the configured `DropPolicy`.

#[cfg(not(feature = "std"))]
use alloc::vec::Vec;

use super::node::DropPolicy;

/// A capacity-bounded `Vec`.  Acts as a FIFO ring-buffer when full.
#[derive(Debug, Clone)]
pub struct BoundedVec<T, const CAP: usize> {
    inner: Vec<T>,
    pub drops: u64,
}

impl<T: Clone, const CAP: usize> BoundedVec<T, CAP> {
    pub fn new() -> Self {
        Self { inner: Vec::with_capacity(CAP), drops: 0 }
    }

    /// Number of items currently in the buffer.
    pub fn len(&self) -> usize { self.inner.len() }

    pub fn is_empty(&self) -> bool { self.inner.is_empty() }

    pub fn is_full_capped(&self, cap: usize) -> bool {
        self.inner.len() >= cap.min(CAP)
    }

    /// Try to push `item`, applying `policy` if the buffer would exceed `cap`
    /// (must be ≤ CAP).  Returns `true` if the item was accepted.
    ///
    /// For `FlushEarly` the caller must drain the buffer **before** calling
    /// this method; here we simply append (the buffer was just cleared).
    pub fn push_capped(&mut self, item: T, policy: DropPolicy, cap: usize) -> bool {
        let effective_cap = cap.min(CAP);
        if self.inner.len() >= effective_cap {
            self.drops += 1;
            match policy {
                DropPolicy::DropOldest | DropPolicy::FlushEarly => {
                    self.inner.remove(0);
                    self.inner.push(item);
                    true
                }
                DropPolicy::DropNewest => false,
            }
        } else {
            self.inner.push(item);
            true
        }
    }

    /// Drain all items into a `Vec` and clear the buffer.
    pub fn drain_all(&mut self) -> Vec<T> {
        let mut out = Vec::with_capacity(self.inner.len());
        out.extend(core::mem::take(&mut self.inner));
        out
    }

    pub fn iter(&self) -> core::slice::Iter<'_, T> { self.inner.iter() }
}

impl<T: Clone, const CAP: usize> Default for BoundedVec<T, CAP> {
    fn default() -> Self { Self::new() }
}
