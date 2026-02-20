//! # RIVR Source Adapters
//!
//! Hardware-friendly glue between real-world interrupt-driven I/O and the
//! single-threaded RIVR [`Engine`].
//!
//! ## Usage pattern (ESP32 / Cortex-M)
//! 1. Allocate a [`RingBuf`] in a shared static or `Mutex<RefCell<…>>`.
//! 2. Push frames from the ISR / RTOS task via `RingBuf::try_push`.
//! 3. In the main loop / RTOS task that owns the engine, call
//!    `RingBuf::drain_into_engine` to move buffered events into the engine.
//!
//! ```rust,ignore
//! static RF_BUF: Mutex<RefCell<RingBuf<[u8; 64], 16>>> =
//!     Mutex::new(RefCell::new(RingBuf::new()));
//!
//! // In ISR:
//! free(|cs| RF_BUF.borrow(cs).borrow_mut().try_push(frame));
//!
//! // In main loop:
//! free(|cs| {
//!     RF_BUF.borrow(cs).borrow_mut()
//!         .drain_into_engine(&mut engine, "rf", Stamp::at(1, lmp_tick));
//! });
//! ```

pub mod ringbuf;

pub use ringbuf::RingBuf;
