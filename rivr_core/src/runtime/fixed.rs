//! # Fixed-capacity stack types for no-alloc / embedded environments
//!
//! [`FixedText<N>`] and [`FixedBytes<N>`] are stack-allocated alternatives to
//! `String` and `Vec<u8>` respectively.  They implement the same surface that
//! [`super::value::Value`] needs so the same `Value` enum can be used without
//! a heap allocator by switching off the `alloc` Cargo feature.
//!
//! ## Overflow behaviour
//! Characters / bytes that would push the buffer past `N` bytes are **silently
//! truncated**.  The `truncated()` flag records whether any data was lost.

// ─────────────────────────────────────────────────────────────────────────────
// FixedText<N>
// ─────────────────────────────────────────────────────────────────────────────

/// Stack-allocated UTF-8 string with a compile-time byte capacity of `N`.
///
/// Silently truncates at a UTF-8 character boundary on overflow.
#[derive(Clone, Copy)]
pub struct FixedText<const N: usize> {
    buf: [u8; N],
    len: usize,
    truncated: bool,
}

impl<const N: usize> FixedText<N> {
    /// Create an empty `FixedText`.
    pub const fn new() -> Self {
        Self {
            buf: [0u8; N],
            len: 0,
            truncated: false,
        }
    }

    /// Build from a string slice, truncating if longer than `N` bytes.
    #[allow(clippy::should_implement_trait)] // intentional: truncating semantics differ from FromStr
    pub fn from_str(s: &str) -> Self {
        let mut this = Self::new();
        this.push_str(s);
        this
    }

    /// Append a `&str`, truncating at a `char` boundary on overflow.
    pub fn push_str(&mut self, s: &str) {
        for ch in s.chars() {
            let enc = ch.len_utf8();
            if self.len + enc > N {
                self.truncated = true;
                break;
            }
            ch.encode_utf8(&mut self.buf[self.len..]);
            self.len += enc;
        }
    }

    /// Append a single ASCII byte (no-op if this overflows).
    pub fn push_ascii(&mut self, b: u8) {
        if self.len < N {
            self.buf[self.len] = b;
            self.len += 1;
        } else {
            self.truncated = true;
        }
    }

    /// View as a `&str`.
    #[inline]
    pub fn as_str(&self) -> &str {
        // SAFETY: We exclusively push valid UTF-8 (via char::encode_utf8 /
        // push_ascii with ASCII-range bytes only).
        unsafe { core::str::from_utf8_unchecked(&self.buf[..self.len]) }
    }

    pub fn len(&self) -> usize {
        self.len
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    pub fn capacity() -> usize {
        N
    }
    pub fn truncated(&self) -> bool {
        self.truncated
    }

    /// Format an `i64` into a `FixedText` (no alloc).
    pub fn from_i64(n: i64) -> Self {
        let mut t = Self::new();
        t.push_i64(n);
        t
    }

    pub fn push_i64(&mut self, mut n: i64) {
        if n < 0 {
            self.push_ascii(b'-');
            // Avoid overflow on i64::MIN
            let mut digits = [0u8; 20];
            let mut pos = 0usize;
            loop {
                // Use wrapping to handle i64::MIN
                digits[pos] = b'0'.wrapping_add((n.wrapping_rem(-10)).unsigned_abs() as u8);
                pos += 1;
                n = n.wrapping_div(-10);
                if n == 0 {
                    break;
                }
                n = -n;
            }
            for &d in digits[..pos].iter().rev() {
                self.push_ascii(d);
            }
        } else {
            let mut digits = [0u8; 20];
            let mut pos = 0usize;
            loop {
                digits[pos] = b'0' + (n % 10) as u8;
                pos += 1;
                n /= 10;
                if n == 0 {
                    break;
                }
            }
            for &d in digits[..pos].iter().rev() {
                self.push_ascii(d);
            }
        }
    }
}

impl<const N: usize> Default for FixedText<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> PartialEq for FixedText<N> {
    fn eq(&self, other: &Self) -> bool {
        self.as_str() == other.as_str()
    }
}
impl<const N: usize> Eq for FixedText<N> {}

impl<const N: usize> PartialEq<str> for FixedText<N> {
    fn eq(&self, other: &str) -> bool {
        self.as_str() == other
    }
}

impl<const N: usize> core::fmt::Debug for FixedText<N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "FixedText({:?})", self.as_str())
    }
}

impl<const N: usize> core::fmt::Display for FixedText<N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.as_str())
    }
}

impl<const N: usize> From<&str> for FixedText<N> {
    fn from(s: &str) -> Self {
        Self::from_str(s)
    }
}

impl<const N: usize> AsRef<str> for FixedText<N> {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}

// core::fmt::Write allows use with write!() macro without alloc
impl<const N: usize> core::fmt::Write for FixedText<N> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.push_str(s);
        Ok(()) // never fail – truncate silently
    }
}

// Serde: serialize as a plain string, deserialize via visitor (no alloc needed)
impl<const N: usize> serde::Serialize for FixedText<N> {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(self.as_str())
    }
}

impl<'de, const N: usize> serde::Deserialize<'de> for FixedText<N> {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        struct V<const M: usize>;
        impl<'de, const M: usize> serde::de::Visitor<'de> for V<M> {
            type Value = FixedText<M>;
            fn expecting(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                write!(f, "a UTF-8 string (max {M} bytes)")
            }
            fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<FixedText<M>, E> {
                Ok(FixedText::from_str(v))
            }
        }
        d.deserialize_str(V::<N>)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FixedBytes<N>
// ─────────────────────────────────────────────────────────────────────────────

/// Stack-allocated byte buffer with compile-time capacity `N`.
#[derive(Clone, Copy)]
pub struct FixedBytes<const N: usize> {
    buf: [u8; N],
    len: usize,
    truncated: bool,
}

impl<const N: usize> FixedBytes<N> {
    pub const fn new() -> Self {
        Self {
            buf: [0u8; N],
            len: 0,
            truncated: false,
        }
    }

    pub fn from_slice(s: &[u8]) -> Self {
        let mut this = Self::new();
        this.push_bytes(s);
        this
    }

    pub fn push_bytes(&mut self, s: &[u8]) {
        let space = N.saturating_sub(self.len);
        let copy_len = s.len().min(space);
        if copy_len < s.len() {
            self.truncated = true;
        }
        self.buf[self.len..self.len + copy_len].copy_from_slice(&s[..copy_len]);
        self.len += copy_len;
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
    pub fn len(&self) -> usize {
        self.len
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    pub fn truncated(&self) -> bool {
        self.truncated
    }
}

impl<const N: usize> Default for FixedBytes<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> PartialEq for FixedBytes<N> {
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}
impl<const N: usize> Eq for FixedBytes<N> {}

impl<const N: usize> core::fmt::Debug for FixedBytes<N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "FixedBytes({:?})", self.as_bytes())
    }
}

impl<const N: usize> core::fmt::Display for FixedBytes<N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "<bytes len={}>", self.len)
    }
}

impl<const N: usize> From<&[u8]> for FixedBytes<N> {
    fn from(s: &[u8]) -> Self {
        Self::from_slice(s)
    }
}

impl<const N: usize> AsRef<[u8]> for FixedBytes<N> {
    fn as_ref(&self) -> &[u8] {
        self.as_bytes()
    }
}

impl<const N: usize> serde::Serialize for FixedBytes<N> {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_bytes(self.as_bytes())
    }
}

impl<'de, const N: usize> serde::Deserialize<'de> for FixedBytes<N> {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        struct V<const M: usize>;
        impl<'de, const M: usize> serde::de::Visitor<'de> for V<M> {
            type Value = FixedBytes<M>;
            fn expecting(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                write!(f, "a byte sequence (max {M} bytes)")
            }
            fn visit_bytes<E: serde::de::Error>(self, v: &[u8]) -> Result<FixedBytes<M>, E> {
                Ok(FixedBytes::from_slice(v))
            }
        }
        d.deserialize_bytes(V::<N>)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers used by value.rs
// ─────────────────────────────────────────────────────────────────────────────

/// Write-format into a `FixedText<N>` using the `core::fmt::Write` impl.
/// Equivalent to `format!(…)` but allocation-free.
#[macro_export]
macro_rules! fixed_format {
    ($n:literal, $($arg:tt)*) => {{
        use core::fmt::Write as _;
        let mut t = $crate::runtime::fixed::FixedText::<$n>::new();
        let _ = write!(t, $($arg)*);
        t
    }};
}
