//! # RIVR Parser (v2)
//!
//! Handles all v2 syntax additions over v1:
//! - `source NAME @CLOCK = kind;`
//! - `window.ticks(N)`, `delay.ticks(N)`, `throttle.ticks(N)`
//! - `budget.airtime(WINDOW_TICKS, DUTY)`
//! - `filter.kind("TAG")`
//! - `map.lower()`, `map.trim()`, `fold.sum()`, `fold.last()`

#[cfg(not(feature = "std"))]
use alloc::{
    boxed::Box,
    format,
    string::{String, ToString},
    vec::Vec,
};

use crate::ast::*;

// ─────────────────────────────────────────────────────────────────────────────
// Error type
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug)]
pub struct ParseError {
    pub pos: usize,
    pub msg: String,
}

impl ParseError {
    fn new(pos: usize, msg: impl Into<String>) -> Self {
        Self {
            pos,
            msg: msg.into(),
        }
    }
}

impl core::fmt::Display for ParseError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "parse error at byte {}: {}", self.pos, self.msg)
    }
}

pub type Result<T> = core::result::Result<T, ParseError>;

// ─────────────────────────────────────────────────────────────────────────────
// Parser
// ─────────────────────────────────────────────────────────────────────────────

struct Parser<'s> {
    src: &'s str,
    pos: usize,
}

impl<'s> Parser<'s> {
    fn new(src: &'s str) -> Self {
        Self { src, pos: 0 }
    }

    // ── cursor helpers ────────────────────────────────────────────────────

    fn peek(&self) -> char {
        self.src[self.pos..].chars().next().unwrap_or('\0')
    }

    fn skip_ws(&mut self) {
        loop {
            while self.pos < self.src.len() && self.src.as_bytes()[self.pos].is_ascii_whitespace() {
                self.pos += 1;
            }
            if self.src[self.pos..].starts_with("//") {
                while self.pos < self.src.len() && self.src.as_bytes()[self.pos] != b'\n' {
                    self.pos += 1;
                }
            } else {
                break;
            }
        }
    }

    fn eat(&mut self, kw: &str) -> bool {
        self.skip_ws();
        if !self.src[self.pos..].starts_with(kw) {
            return false;
        }
        let after = self.pos + kw.len();
        // Do not consume if it is just a prefix of a longer identifier.
        let last_alnum = kw
            .chars()
            .last()
            .map(|c| c.is_alphanumeric() || c == '_')
            .unwrap_or(false);
        if last_alnum {
            let next = self.src[after..].chars().next().unwrap_or('\0');
            if next.is_alphanumeric() || next == '_' {
                return false;
            }
        }
        self.pos += kw.len();
        true
    }

    fn expect(&mut self, kw: &str) -> Result<()> {
        if self.eat(kw) {
            return Ok(());
        }
        self.skip_ws();
        let ctx: String = self.src[self.pos..].chars().take(12).collect();
        Err(ParseError::new(
            self.pos,
            format!("expected `{kw}`, found `{ctx}…`"),
        ))
    }

    fn at_end(&mut self) -> bool {
        self.skip_ws();
        self.pos >= self.src.len()
    }

    // ── token parsers ─────────────────────────────────────────────────────

    fn parse_ident(&mut self) -> Result<String> {
        self.skip_ws();
        let start = self.pos;
        while self.pos < self.src.len() {
            let b = self.src.as_bytes()[self.pos];
            if b.is_ascii_alphanumeric() || b == b'_' {
                self.pos += 1;
            } else {
                break;
            }
        }
        if self.pos == start {
            return Err(ParseError::new(self.pos, "expected identifier"));
        }
        Ok(self.src[start..self.pos].to_string())
    }

    fn parse_uint(&mut self) -> Result<u64> {
        self.skip_ws();
        let start = self.pos;
        while self.pos < self.src.len() && self.src.as_bytes()[self.pos].is_ascii_digit() {
            self.pos += 1;
        }
        if self.pos == start {
            return Err(ParseError::new(self.pos, "expected integer"));
        }
        self.src[start..self.pos]
            .parse::<u64>()
            .map_err(|e| ParseError::new(start, e.to_string()))
    }

    fn parse_positive_uint(&mut self, what: &str) -> Result<u64> {
        let value = self.parse_uint()?;
        if value == 0 {
            return Err(ParseError::new(self.pos, format!("{what} must be > 0")));
        }
        Ok(value)
    }

    fn parse_float(&mut self) -> Result<f64> {
        self.skip_ws();
        let start = self.pos;
        while self.pos < self.src.len() {
            let b = self.src.as_bytes()[self.pos];
            if b.is_ascii_digit() || b == b'.' {
                self.pos += 1;
            } else {
                break;
            }
        }
        if self.pos == start {
            return Err(ParseError::new(self.pos, "expected number"));
        }
        self.src[start..self.pos]
            .parse::<f64>()
            .map_err(|e| ParseError::new(start, e.to_string()))
    }

    fn parse_string(&mut self) -> Result<String> {
        self.skip_ws();
        self.expect("\"")?;
        let mut result = String::new();
        loop {
            if self.pos >= self.src.len() {
                return Err(ParseError::new(self.pos, "unterminated string literal"));
            }
            let b = self.src.as_bytes()[self.pos];
            if b == b'"' {
                self.pos += 1;
                break;
            }
            if b == b'\\' {
                self.pos += 1;
                match self.src.as_bytes().get(self.pos).copied().unwrap_or(0) {
                    b'n' => {
                        result.push('\n');
                        self.pos += 1;
                    }
                    b't' => {
                        result.push('\t');
                        self.pos += 1;
                    }
                    b'\\' => {
                        result.push('\\');
                        self.pos += 1;
                    }
                    b'"' => {
                        result.push('"');
                        self.pos += 1;
                    }
                    c => {
                        return Err(ParseError::new(
                            self.pos,
                            format!("unknown escape \\{}", c as char),
                        ))
                    }
                }
            } else {
                result.push(self.src[self.pos..].chars().next().unwrap_or('\0'));
                self.pos += 1;
            }
        }
        Ok(result)
    }

    // ── dotted name helper (reads `a.b.c` or `a_b`) ──────────────────────

    fn read_dotted_name(&mut self) -> String {
        self.skip_ws();
        let start = self.pos;
        while self.pos < self.src.len() {
            let b = self.src.as_bytes()[self.pos];
            if b.is_ascii_alphanumeric() || b == b'_' || b == b'.' {
                self.pos += 1;
            } else {
                break;
            }
        }
        self.src[start..self.pos].to_string()
    }

    // ── pipe-op parser ────────────────────────────────────────────────────

    fn parse_pipe_op(&mut self) -> Result<PipeOp> {
        self.skip_ws();
        let name = self.read_dotted_name();

        // Helper: skip optional named-param `key=` before a value.
        macro_rules! skip_named {
            ($key:expr) => {
                let _ = self.eat($key) && self.eat("=");
            };
        }

        match name.as_str() {
            "map.upper" => {
                self.expect("()")?;
                Ok(PipeOp::MapUpper)
            }
            "map.lower" => {
                self.expect("()")?;
                Ok(PipeOp::MapLower)
            }
            "map.trim" => {
                self.expect("()")?;
                Ok(PipeOp::MapTrim)
            }
            "filter.nonempty" => {
                self.expect("()")?;
                Ok(PipeOp::FilterNonempty)
            }
            "fold.count" => {
                self.expect("()")?;
                Ok(PipeOp::FoldCount)
            }
            "fold.sum" => {
                self.expect("()")?;
                Ok(PipeOp::FoldSum)
            }
            "fold.last" => {
                self.expect("()")?;
                Ok(PipeOp::FoldLast)
            }

            "filter.kind" => {
                self.expect("(")?;
                let kind = self.parse_string()?;
                self.expect(")")?;
                Ok(PipeOp::FilterKind(kind))
            }

            "filter.pkt_type" => {
                self.expect("(")?;
                let t = self.parse_uint()?;
                if t > 255 {
                    return Err(ParseError::new(
                        self.pos,
                        "filter.pkt_type argument must be 0–255",
                    ));
                }
                self.expect(")")?;
                Ok(PipeOp::FilterPktType(t as u8))
            }

            "window.ms" => {
                self.expect("(")?;
                let ms = self.parse_positive_uint("window.ms duration")?;
                self.expect(")")?;
                Ok(PipeOp::WindowMs(ms))
            }
            "throttle.ms" => {
                self.expect("(")?;
                let ms = self.parse_positive_uint("throttle.ms interval")?;
                self.expect(")")?;
                Ok(PipeOp::ThrottleMs(ms))
            }
            "debounce.ms" => {
                self.expect("(")?;
                let ms = self.parse_positive_uint("debounce.ms delay")?;
                self.expect(")")?;
                Ok(PipeOp::DebounceMs(ms))
            }

            // window.ticks(N)  or  window.ticks(N, CAP, "policy")
            "window.ticks" => {
                self.expect("(")?;
                let n = self.parse_positive_uint("window.ticks duration")?;
                // Optional: , CAP, "policy"
                if self.eat(",") {
                    let cap = self.parse_positive_uint("window.ticks capacity")? as usize;
                    self.expect(",")?;
                    let pol_str = self.parse_string()?;
                    self.expect(")")?;
                    let policy = match pol_str.as_str() {
                        "drop_oldest" | "DropOldest" => crate::ast::WindowPolicy::DropOldest,
                        "drop_newest" | "DropNewest" => crate::ast::WindowPolicy::DropNewest,
                        "flush_early" | "FlushEarly" => crate::ast::WindowPolicy::FlushEarly,
                        other => {
                            return Err(ParseError::new(
                                self.pos,
                                format!("unknown window policy `{other}`"),
                            ))
                        }
                    };
                    Ok(PipeOp::WindowTicksCapped {
                        ticks: n,
                        cap,
                        policy,
                    })
                } else {
                    self.expect(")")?;
                    Ok(PipeOp::WindowTicks(n))
                }
            }
            "throttle.ticks" => {
                self.expect("(")?;
                let n = self.parse_positive_uint("throttle.ticks interval")?;
                self.expect(")")?;
                Ok(PipeOp::ThrottleTicks(n))
            }
            "delay.ticks" => {
                self.expect("(")?;
                let n = self.parse_positive_uint("delay.ticks delay")?;
                self.expect(")")?;
                Ok(PipeOp::DelayTicks(n))
            }

            "budget" => {
                self.expect("(")?;
                let rate = self.parse_float()?;
                self.expect(",")?;
                let burst = self.parse_uint()?;
                self.expect(")")?;
                Ok(PipeOp::Budget { rate, burst })
            }

            "budget.airtime" => {
                // budget.airtime(WINDOW_TICKS, DUTY)
                // or budget.airtime(ms=360000, duty=0.10)
                self.expect("(")?;
                skip_named!("ms");
                let window_ticks = self.parse_positive_uint("budget.airtime window")?;
                self.expect(",")?;
                skip_named!("duty");
                let duty = self.parse_float()?;
                self.expect(")")?;
                Ok(PipeOp::BudgetAirtime { window_ticks, duty })
            }

            "budget.toa_us" => {
                // budget.toa_us(WINDOW_MS, DUTY, TOA_US)
                // or budget.toa_us(window_ms=360000, duty=0.10, toa_us=400)
                self.expect("(")?;
                skip_named!("window_ms");
                let window_ms = self.parse_positive_uint("budget.toa_us window")?;
                self.expect(",")?;
                skip_named!("duty");
                let duty = self.parse_float()?;
                self.expect(",")?;
                skip_named!("toa_us");
                let toa_us = self.parse_uint()?;
                self.expect(")")?;
                Ok(PipeOp::BudgetToaUs {
                    window_ms,
                    duty,
                    toa_us,
                })
            }

            "tag" => {
                self.expect("(")?;
                let label = self.parse_string()?;
                self.expect(")")?;
                Ok(PipeOp::Tag(label))
            }

            other => Err(ParseError::new(
                self.pos,
                format!("unknown pipe operator `{other}`"),
            )),
        }
    }

    // ── expression parser ─────────────────────────────────────────────────

    fn parse_primary(&mut self) -> Result<Expr> {
        self.skip_ws();
        if self.eat("merge") {
            self.expect("(")?;
            let a = self.parse_ident()?;
            self.expect(",")?;
            let b = self.parse_ident()?;
            self.expect(")")?;
            return Ok(Expr::Merge(a, b));
        }
        if self.peek() == '"' {
            return Ok(Expr::Lit(Literal::Str(self.parse_string()?)));
        }
        if self.peek().is_ascii_digit() {
            return Ok(Expr::Lit(Literal::Int(self.parse_uint()? as i64)));
        }
        Ok(Expr::Ident(self.parse_ident()?))
    }

    fn parse_expr(&mut self) -> Result<Expr> {
        let mut lhs = self.parse_primary()?;
        loop {
            self.skip_ws();
            if self.src[self.pos..].starts_with("|>") {
                self.pos += 2;
                let op = self.parse_pipe_op()?;
                lhs = Expr::Pipe {
                    lhs: Box::new(lhs),
                    op,
                };
            } else {
                break;
            }
        }
        Ok(lhs)
    }

    // ── sink parser ───────────────────────────────────────────────────────

    fn parse_sink(&mut self) -> Result<Sink> {
        self.skip_ws();
        let name = self.read_dotted_name();
        match name.as_str() {
            "io.usb.print" => {
                self.expect("(")?;
                let a = self.parse_ident()?;
                self.expect(")")?;
                Ok(Sink::UsbPrint(a))
            }
            "io.lora.tx" => {
                self.expect("(")?;
                let a = self.parse_ident()?;
                self.expect(")")?;
                Ok(Sink::LoraTx(a))
            }
            "io.lora.beacon" => {
                self.expect("(")?;
                let a = self.parse_ident()?;
                self.expect(")")?;
                Ok(Sink::LoraBeacon(a))
            }
            "io.debug.dump" => {
                self.expect("(")?;
                let a = self.parse_ident()?;
                self.expect(")")?;
                Ok(Sink::DebugDump(a))
            }
            other => Err(ParseError::new(self.pos, format!("unknown sink `{other}`"))),
        }
    }

    // ── source-kind parser ────────────────────────────────────────────────

    fn parse_source_kind(&mut self) -> Result<SourceKind> {
        let name = self.parse_ident()?;
        match name.as_str() {
            "usb" => Ok(SourceKind::Usb),
            "lora" => Ok(SourceKind::Lora),
            "rf" => Ok(SourceKind::Rf),
            "programmatic" => Ok(SourceKind::Programmatic),
            "timer" => {
                self.expect("(")?;
                let interval_ms = self.parse_uint()?;
                if interval_ms == 0 {
                    return Err(ParseError::new(self.pos, "timer interval must be > 0"));
                }
                self.expect(")")?;
                Ok(SourceKind::Timer { interval_ms })
            }
            other => Err(ParseError::new(
                self.pos,
                format!("unknown source kind `{other}`"),
            )),
        }
    }

    // ── statement parsers ─────────────────────────────────────────────────

    fn parse_stmt(&mut self) -> Result<Stmt> {
        self.skip_ws();

        if self.eat("source") {
            let name = self.parse_ident()?;
            // Optional `@clock_name` annotation.
            let clock = if self.eat("@") {
                let cname = self.parse_ident()?;
                Some(ClockAnnotation { name: cname })
            } else {
                None
            };
            self.expect("=")?;
            let kind = self.parse_source_kind()?;
            self.expect(";")?;
            return Ok(Stmt::Source { name, clock, kind });
        }

        if self.eat("let") {
            let name = self.parse_ident()?;
            self.expect("=")?;
            let expr = self.parse_expr()?;
            self.expect(";")?;
            return Ok(Stmt::Let { name, expr });
        }

        if self.eat("emit") {
            self.expect("{")?;
            let mut sinks = Vec::new();
            loop {
                self.skip_ws();
                if self.peek() == '}' {
                    self.pos += 1;
                    break;
                }
                sinks.push(self.parse_sink()?);
                self.expect(";")?;
            }
            return Ok(Stmt::Emit { sinks });
        }

        let ctx: String = self.src[self.pos..].chars().take(16).collect();
        Err(ParseError::new(
            self.pos,
            format!("expected `source`, `let`, or `emit`; found `{ctx}…`"),
        ))
    }

    fn parse_program(&mut self) -> Result<Program> {
        let mut stmts = Vec::new();
        while !self.at_end() {
            stmts.push(self.parse_stmt()?);
        }
        Ok(Program { stmts })
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// Parse a RIVR source string into a [`Program`] AST.
pub fn parse(src: &str) -> Result<Program> {
    Parser::new(src).parse_program()
}

// ─────────────────────────────────────────────────────────────────────────────
// Unit tests
// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    /// Empty source string produces an empty program (zero statements).
    #[test]
    fn parse_empty_program() {
        let prog = parse("").expect("empty program must parse");
        assert!(prog.stmts.is_empty());
    }

    /// A comment-only file also produces an empty program.
    #[test]
    fn parse_comment_only() {
        let prog = parse("// just a comment\n").expect("comment-only must parse");
        assert!(prog.stmts.is_empty());
    }

    /// A minimal valid program: one source, one let, one emit.
    #[test]
    fn parse_minimal_valid_program() {
        let src = r#"
            source radio = rf;
            let stream = radio;
            emit { io.lora.tx(stream); }
        "#;
        let prog = parse(src).expect("minimal program must parse");
        assert_eq!(prog.stmts.len(), 3, "expected source + let + emit");
    }

    /// A source with a clock annotation parses without error.
    #[test]
    fn parse_source_with_clock() {
        let src = "source rf_rx @lmp = rf;";
        parse(src).expect("source with clock annotation must parse");
    }

    /// A pipe-chain with multiple operators parses without error.
    #[test]
    fn parse_pipe_chain() {
        let src = r#"
            source kbd = usb;
            let filtered = kbd |> map.upper() |> filter.nonempty();
            emit { io.usb.print(filtered); }
        "#;
        parse(src).expect("pipe chain must parse");
    }

    /// An unrecognised token must yield a `ParseError` (not a panic).
    #[test]
    fn parse_invalid_token_returns_error() {
        let result = parse("@@@@");
        assert!(result.is_err(), "invalid token must produce a ParseError");
    }

    /// A source declaration with a missing semicolon must fail gracefully.
    #[test]
    fn parse_missing_semicolon_returns_error() {
        let result = parse("source x = usb");
        assert!(result.is_err(), "missing ';' must produce a ParseError");
    }

    /// An emit block that is never closed (missing `}`) must fail gracefully.
    #[test]
    fn parse_unterminated_emit_returns_error() {
        let result = parse("source x = usb; emit { io.usb.print(x);");
        assert!(
            result.is_err(),
            "unterminated emit block must produce a ParseError"
        );
    }

    #[test]
    fn parse_rejects_zero_timed_parameters() {
        for src in [
            "source rf = rf; let x = rf |> window.ms(0);",
            "source rf = rf; let x = rf |> throttle.ms(0);",
            "source rf = rf; let x = rf |> debounce.ms(0);",
            "source rf = rf; let x = rf |> window.ticks(0);",
            "source rf = rf; let x = rf |> window.ticks(1, 0, \"drop_oldest\");",
            "source rf = rf; let x = rf |> throttle.ticks(0);",
            "source rf = rf; let x = rf |> delay.ticks(0);",
            "source rf = rf; let x = rf |> budget.airtime(0, 0.10);",
            "source rf = rf; let x = rf |> budget.toa_us(0, 0.10, 400);",
        ] {
            assert!(parse(src).is_err(), "expected parse error for `{src}`");
        }
    }
}
