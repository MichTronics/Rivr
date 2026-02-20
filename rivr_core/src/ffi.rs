// Safety: ENGINE_SLOT is a single-core singleton; ISRs never touch it.
// Suppress the Rust 2024 static_mut_refs lint for the whole module.
#![allow(static_mut_refs)]

// ffi.rs – C-ABI exports from rivr_core for the embedded firmware.
//
// BUILD
// ─────
// Compile rivr_core as a static library (`crate-type = ["staticlib"]` in
// Cargo.toml) with the `ffi` feature enabled.  `std` is on by default and is
// the recommended path for ESP32/ESP-IDF builds (espup toolchain provides std).
//
//   cargo +esp build -p rivr_core \
//     --target xtensa-esp32-espidf \
//     --features ffi --release
//
// For bare-metal no_std targets (WIP), see the `embedded` feature.
//
// SINK DISPATCH
// ─────────────
// Instead of storing Rust closures (requires alloc), we call back into the
// C-side `rivr_emit_dispatch(sink_name, value)` function pointer.
// The C firmware registers this during rivr_embed_init().
#![allow(static_mut_refs)]

use core::ffi::{c_char, CStr};

// Under no_std + alloc, String/format must be imported from alloc explicitly.
#[cfg(all(not(feature = "std"), feature = "alloc"))]
use alloc::string::String;

use crate::{compile, parse, Engine, Event, Stamp, Value};
use crate::runtime::value::StrBuf;   // StrBuf = String (alloc) or FixedText<64> (no-alloc)
#[cfg(not(feature = "alloc"))]
use crate::runtime::fixed::FixedText;

// ── C-ABI types (must match rivr_embed.h) ────────────────────────────────────

#[repr(C)]
pub struct CStamp {
    pub clock: u8,
    pub tick:  u64,
}

#[repr(u8)]
pub enum CValTag {
    Unit  = 0,
    Int   = 1,
    Bool  = 2,
    Str   = 3,
    Bytes = 4,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct CFixedStr {
    pub buf: [u8; 128],
    pub len: u8,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct CFixedBytes {
    pub buf: [u8; 256],
    pub len: u16,
}

#[repr(C)]
pub union CValUnion {
    pub as_int:   i64,
    pub as_bool:  bool,
    pub as_str:   CFixedStr,
    pub as_bytes: CFixedBytes,
}

#[repr(C)]
pub struct CValue {
    pub tag: u8,                 // CValTag discriminant
    pub _pad: [u8; 3],
    pub data: CValUnion,
}

#[repr(C)]
pub struct CEvent {
    pub stamp:    CStamp,
    pub v:        CValue,
    pub kind_tag: [u8; 32],
    pub seq:      u32,
}

// ── Emit dispatch callback type ───────────────────────────────────────────────
// The C side provides this function (rivr_emit_dispatch in rivr_embed.c).
type EmitDispatchFn = unsafe extern "C" fn(sink_name: *const c_char, v: *const CValue);

// ── Static engine slot (one per firmware) ─────────────────────────────────────
// No heap required: the Engine is stored in BSS.  16 KB fits the pipeline.
// Using MaybeUninit to avoid zero-initialising the Engine struct (which is large).
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicBool, Ordering};

static mut ENGINE_SLOT: MaybeUninit<Engine> = MaybeUninit::uninit();
static ENGINE_READY: AtomicBool = AtomicBool::new(false);

static mut EMIT_DISPATCH: Option<EmitDispatchFn> = None;

// ── FFI exports ──────────────────────────────────────────────────────────────

/// Initialise the engine from a null-terminated RIVR program string in flash.
///
/// Returns 0 on success, -1 on parse error, -2 on compile error.
#[no_mangle]
pub unsafe extern "C" fn rivr_engine_init(program_src: *const c_char) -> i32 {
    if program_src.is_null() { return -1; }

    let src = match CStr::from_ptr(program_src).to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };

    let program = match parse(src) {
        Ok(p) => p,
        Err(_) => return -1,
    };

    let (engine, _warns) = match compile(&program) {
        Ok(pair) => pair,
        Err(_) => return -2,
    };

    ENGINE_SLOT.write(engine);
    ENGINE_READY.store(true, Ordering::Release);
    0
}

/// Inject one event into the named source queue.
///
/// Returns 0 on success, -1 if source not found, -2 if queue full.
#[no_mangle]
pub unsafe extern "C" fn rivr_inject_event(
    source_name: *const c_char,
    event: *const CEvent,
) -> i32 {
    if !ENGINE_READY.load(Ordering::Acquire) { return -1; }
    if source_name.is_null() || event.is_null() { return -1; }

    let name = match CStr::from_ptr(source_name).to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };

    let ev_c = &*event;
    let stamp = Stamp::at(ev_c.stamp.clock, ev_c.stamp.tick);

    let value = match ev_c.v.tag {
        1 /* Int */  => Value::Int(ev_c.v.data.as_int),
        2 /* Bool */ => Value::Bool(ev_c.v.data.as_bool),
        3 /* Str */  => {
            let s = &ev_c.v.data.as_str;
            let len = s.len as usize;
            let text = core::str::from_utf8(&s.buf[..len]).unwrap_or("");
            // StrBuf = String under alloc, FixedText<64> otherwise.
            #[cfg(feature = "alloc")]
            let strbuf: StrBuf = String::from(text);
            #[cfg(not(feature = "alloc"))]
            let strbuf: StrBuf = { let mut ft = FixedText::<64>::new(); ft.push_str(text); ft };
            Value::Str(strbuf)
        }
        4 /* Bytes */ => {
            let b = &ev_c.v.data.as_bytes;
            let len = b.len as usize;
            // ByteBuf = Vec<u8> under alloc, FixedBytes<64> otherwise.
            #[cfg(feature = "alloc")]
            {
                use crate::runtime::value::ByteBuf;
                let mut buf: ByteBuf = ByteBuf::with_capacity(len);
                buf.extend_from_slice(&b.buf[..len]);
                Value::Bytes(buf)
            }
            #[cfg(not(feature = "alloc"))]
            {
                use crate::runtime::fixed::FixedBytes;
                let copy_len = len.min(64);
                let fb = FixedBytes::<64>::from_slice(&b.buf[..copy_len]);
                Value::Bytes(fb)
            }
        }
        _ => Value::Unit,
    };

    // Extract kind_tag (optional trace label; not used for filter.kind routing
    // which operates on Value::kind_tag() = prefix before ':')
    let tag_end = ev_c.kind_tag.iter().position(|&b| b == 0).unwrap_or(32);
    let tag_str = core::str::from_utf8(&ev_c.kind_tag[..tag_end]).ok();

    // Event::tag is Option<String>.  Available under std or alloc; None otherwise.
    #[cfg(feature = "alloc")]
    let event_tag: Option<String> = tag_str.filter(|s| !s.is_empty()).map(String::from);
    #[cfg(not(feature = "alloc"))]
    let event_tag: Option<String> = None;

    let rivr_event = Event {
        stamp,
        v: value,
        tag: event_tag,
        seq: ev_c.seq,
    };

    let engine = ENGINE_SLOT.assume_init_mut();
    match engine.inject(name, rivr_event) {
        Ok(_) => 0,
        Err(_) => -2,
    }
}

/// Run the engine for up to `max_steps` cycles.  Returns steps taken.
#[no_mangle]
pub unsafe extern "C" fn rivr_engine_run(max_steps: u32) -> u32 {
    if !ENGINE_READY.load(Ordering::Acquire) { return 0; }
    let engine = ENGINE_SLOT.assume_init_mut();
    engine.run(max_steps as usize) as u32
}

/// Return the current tick for the given clock index.
#[no_mangle]
pub unsafe extern "C" fn rivr_engine_clock_now(clock_id: u8) -> u64 {
    if !ENGINE_READY.load(Ordering::Acquire) { return 0; }
    let engine = ENGINE_SLOT.assume_init_ref();
    engine.clock_now[clock_id as usize & 7]
}

/// Register the C-side emit dispatch callback.
/// Call this BEFORE rivr_engine_init().
#[no_mangle]
pub unsafe extern "C" fn rivr_set_emit_dispatch(f: EmitDispatchFn) {
    EMIT_DISPATCH = Some(f);
}

// ── Internal: called by the Engine whenever an emit fires ────────────────────
// This is wired up via Engine::set_emit_hook() (to be added to engine.rs).
#[doc(hidden)]
pub unsafe fn ffi_emit_hook(sink_name: &str, v: &Value) {
    let dispatch = match EMIT_DISPATCH { Some(f) => f, None => return };

    // Convert sink_name to null-terminated C string (stack)
    let mut name_buf = [0u8; 32];
    let name_len = sink_name.len().min(31);
    name_buf[..name_len].copy_from_slice(&sink_name.as_bytes()[..name_len]);

    // Convert Value to CValue
    let cv = value_to_c(v);

    dispatch(name_buf.as_ptr() as *const c_char, &cv);
}

fn value_to_c(v: &Value) -> CValue {
    match v {
        Value::Int(n) => CValue {
            tag: 1, _pad: [0;3],
            data: CValUnion { as_int: *n },
        },
        Value::Bool(b) => CValue {
            tag: 2, _pad: [0;3],
            data: CValUnion { as_bool: *b },
        },
        Value::Str(s) => {
            // StrBuf is String (alloc) or FixedText<64>; both deref to str.
            let sr: &str = s.as_ref();
            let bytes = sr.as_bytes();
            let len   = bytes.len().min(127) as u8;
            let mut buf = [0u8; 128];
            buf[..len as usize].copy_from_slice(&bytes[..len as usize]);
            CValue {
                tag: 3, _pad: [0;3],
                data: CValUnion { as_str: CFixedStr { buf, len } },
            }
        }
        _ => CValue { tag: 0, _pad: [0;3], data: CValUnion { as_int: 0 } },
    }
}

// ── Embedded no_std support ───────────────────────────────────────────────────
//
// When building the staticlib for ESP32 (no std, has alloc via this crate):
//   • We delegate the global allocator to libc malloc/free provided by ESP-IDF.
//   • We define a minimal panic handler that aborts (ESP-IDF handles the reset).
//
// Both are gated on `not(feature = "std") + not(test)` so host builds and
// unit-test harnesses use the normal Rust allocator / test panic handler.

#[cfg(all(not(feature = "std"), not(test)))]
mod embedded_rt {
    use core::alloc::{GlobalAlloc, Layout};
    use core::panic::PanicInfo;

    /// Thin wrapper that forwards to ESP-IDF's libc malloc / free.
    struct LibcAlloc;

    unsafe impl GlobalAlloc for LibcAlloc {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            extern "C" { fn malloc(size: usize) -> *mut u8; }
            malloc(layout.size())
        }
        unsafe fn dealloc(&self, ptr: *mut u8, _: Layout) {
            extern "C" { fn free(ptr: *mut u8); }
            free(ptr)
        }
        unsafe fn realloc(&self, ptr: *mut u8, _layout: Layout, new_size: usize) -> *mut u8 {
            extern "C" { fn realloc(ptr: *mut u8, size: usize) -> *mut u8; }
            realloc(ptr, new_size)
        }
    }

    #[global_allocator]
    static A: LibcAlloc = LibcAlloc;

    /// Abort on panic.  ESP-IDF's watchdog or the hardfault handler will reset.
    #[panic_handler]
    fn panic(_: &PanicInfo) -> ! {
        extern "C" { fn abort() -> !; }
        unsafe { abort() }
    }
}
