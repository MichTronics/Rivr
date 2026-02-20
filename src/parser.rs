//! # RIVR Parser
//!
//! A hand-written recursive-descent parser that converts raw source text into
//! a [`Program`] AST.  The parser is intentionally simple – RIVR has very few
//! constructs – but it reports accurate byte-offset errors to help embedded
//! developers during development.
//!
//! ## Tokenisation strategy
//! The parser operates on a `&str` slice and advances a `pos: usize` cursor.
//! There is no separate lexer phase; tokens are recognised inline.  This
//! keeps the binary small (important for ESP32 deployments) without losing
//! clarity.

use crate::ast::*;

// ─────────────────────────────────────────────────────────────────────────────
// Error type
// ─────────────────────────────────────────────────────────────────────────────

/// A parse error with the byte-offset at which it occurred.
#[derive(Debug)]
pub struct ParseError {
    pub pos: usize,
    pub msg: String,
}

impl ParseError {
    fn new(pos: usize, msg: impl Into<String>) -> Self {
        Self { pos, msg: msg.into() }
    }
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Parse error at byte {}: {}", self.pos, self.msg)
    }
}

pub type Result<T> = std::result::Result<T, ParseError>;

// ─────────────────────────────────────────────────────────────────────────────
// Parser state
// ─────────────────────────────────────────────────────────────────────────────

struct Parser<'src> {
    src: &'src str,
    pos: usize,
}

impl<'src> Parser<'src> {
    fn new(src: &'src str) -> Self {
        Self { src, pos: 0 }
    }

    // ── low-level cursor helpers ──────────────────────────────────────────

    /// Current character, or `\0` at EOF.
    fn peek(&self) -> char {
        self.src[self.pos..].chars().next().unwrap_or('\0')
    }

    /// Consume whitespace and `//` line comments.
    fn skip_ws(&mut self) {
        loop {
            // skip whitespace
            while self.pos < self.src.len()
                && self.src.as_bytes()[self.pos].is_ascii_whitespace()
            {
                self.pos += 1;
            }
            // consume line comment
            if self.src[self.pos..].starts_with("//") {
                while self.pos < self.src.len()
                    && self.src.as_bytes()[self.pos] != b'\n'
                {
                    self.pos += 1;
                }
            } else {
                break;
            }
        }
    }

    /// Try to consume the literal text `kw`.  Returns true on success and
    /// advances the cursor; leaves the cursor unchanged on failure.
    fn eat(&mut self, kw: &str) -> bool {
        self.skip_ws();
        if self.src[self.pos..].starts_with(kw) {
            // Make sure it is not just a prefix of a longer identifier.
            let after = self.pos + kw.len();
            let next_ch = self.src[after..].chars().next().unwrap_or('\0');
            if kw.chars().last().map(|c| c.is_alphanumeric() || c == '_').unwrap_or(false)
                && (next_ch.is_alphanumeric() || next_ch == '_')
            {
                return false;
            }
            self.pos += kw.len();
            true
        } else {
            false
        }
    }

    /// Consume the literal `kw` or emit a parse error.
    fn expect(&mut self, kw: &str) -> Result<()> {
        if self.eat(kw) {
            Ok(())
        } else {
            self.skip_ws();
            Err(ParseError::new(
                self.pos,
                format!("expected `{kw}`, found `{}`", &self.src[self.pos..].chars().take(8).collect::<String>()),
            ))
        }
    }

    fn at_end(&mut self) -> bool {
        self.skip_ws();
        self.pos >= self.src.len()
    }

    // ── token parsers ─────────────────────────────────────────────────────

    /// Parse an ASCII identifier `[a-zA-Z_][a-zA-Z0-9_]*`.
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

    /// Parse an unsigned integer literal.
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

    /// Parse a floating-point literal (e.g. `1.5`).
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

    /// Parse a double-quoted string literal.
    fn parse_string(&mut self) -> Result<String> {
        self.skip_ws();
        self.expect("\"")?;
        let _start = self.pos;
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
                    b'n'  => { result.push('\n'); self.pos += 1; }
                    b't'  => { result.push('\t'); self.pos += 1; }
                    b'\\' => { result.push('\\'); self.pos += 1; }
                    b'"'  => { result.push('"');  self.pos += 1; }
                    other => {
                        return Err(ParseError::new(
                            self.pos,
                            format!("unknown escape \\{}", other as char),
                        ));
                    }
                }
            } else {
                // Push the character at the *current* cursor position.
                result.push(self.src[self.pos..].chars().next().unwrap());
                self.pos += 1;
            }
        }
        Ok(result)
    }

    // ── pipe-op parser ────────────────────────────────────────────────────

    /// Parse one `name.qualifier(args)` pipe operator that follows a `|>`.
    fn parse_pipe_op(&mut self) -> Result<PipeOp> {
        self.skip_ws();

        // We read the dotted name without using `eat` (which treats '.' as a
        // non-identifier boundary) so we can distinguish `map.upper` vs
        // other idents.
        let start = self.pos;
        while self.pos < self.src.len() {
            let b = self.src.as_bytes()[self.pos];
            if b.is_ascii_alphanumeric() || b == b'_' || b == b'.' {
                self.pos += 1;
            } else {
                break;
            }
        }
        let name = &self.src[start..self.pos];

        match name {
            "map.upper" => {
                self.expect("()")?;
                Ok(PipeOp::MapUpper)
            }
            "filter.nonempty" => {
                self.expect("()")?;
                Ok(PipeOp::FilterNonempty)
            }
            "fold.count" => {
                self.expect("()")?;
                Ok(PipeOp::FoldCount)
            }
            "window.ms" => {
                self.expect("(")?;
                let ms = self.parse_uint()?;
                self.expect(")")?;
                Ok(PipeOp::WindowMs(ms))
            }
            "throttle.ms" => {
                self.expect("(")?;
                let ms = self.parse_uint()?;
                self.expect(")")?;
                Ok(PipeOp::ThrottleMs(ms))
            }
            "debounce.ms" => {
                self.expect("(")?;
                let ms = self.parse_uint()?;
                self.expect(")")?;
                Ok(PipeOp::DebounceMs(ms))
            }
            "budget" => {
                self.expect("(")?;
                let rate = self.parse_float()?;
                self.expect(",")?;
                let burst = self.parse_uint()?;
                self.expect(")")?;
                Ok(PipeOp::Budget { rate, burst })
            }
            "tag" => {
                self.expect("(")?;
                let label = self.parse_string()?;
                self.expect(")")?;
                Ok(PipeOp::Tag(label))
            }
            other => Err(ParseError::new(
                start,
                format!("unknown pipe operator `{other}`"),
            )),
        }
    }

    // ── expression parser ─────────────────────────────────────────────────

    /// Parse a primary expression: `merge(a,b)`, an identifier, or a literal.
    fn parse_primary(&mut self) -> Result<Expr> {
        self.skip_ws();

        // merge(a, b)
        if self.eat("merge") {
            self.expect("(")?;
            let a = self.parse_ident()?;
            self.expect(",")?;
            let b = self.parse_ident()?;
            self.expect(")")?;
            return Ok(Expr::Merge(a, b));
        }

        // String literal
        if self.peek() == '"' {
            let s = self.parse_string()?;
            return Ok(Expr::Lit(Literal::Str(s)));
        }

        // Integer literal
        if self.peek().is_ascii_digit() {
            let i = self.parse_uint()?;
            return Ok(Expr::Lit(Literal::Int(i as i64)));
        }

        // Identifier (stream reference)
        let name = self.parse_ident()?;
        Ok(Expr::Ident(name))
    }

    /// Parse a full expression: a primary followed by zero or more `|> op`.
    fn parse_expr(&mut self) -> Result<Expr> {
        let mut lhs = self.parse_primary()?;

        loop {
            self.skip_ws();
            if self.src[self.pos..].starts_with("|>") {
                self.pos += 2; // consume `|>`
                let op = self.parse_pipe_op()?;
                lhs = Expr::Pipe { lhs: Box::new(lhs), op };
            } else {
                break;
            }
        }

        Ok(lhs)
    }

    // ── sink parser (inside emit blocks) ─────────────────────────────────

    fn parse_sink(&mut self) -> Result<Sink> {
        self.skip_ws();
        let start = self.pos;
        // read dotted name
        while self.pos < self.src.len() {
            let b = self.src.as_bytes()[self.pos];
            if b.is_ascii_alphanumeric() || b == b'_' || b == b'.' {
                self.pos += 1;
            } else {
                break;
            }
        }
        let name = &self.src[start..self.pos];
        match name {
            "io.usb.print" => {
                self.expect("(")?;
                let arg = self.parse_ident()?;
                self.expect(")")?;
                Ok(Sink::UsbPrint(arg))
            }
            "io.lora.tx" => {
                self.expect("(")?;
                let arg = self.parse_ident()?;
                self.expect(")")?;
                Ok(Sink::LoraTx(arg))
            }
            "io.debug.dump" => {
                self.expect("(")?;
                let arg = self.parse_ident()?;
                self.expect(")")?;
                Ok(Sink::DebugDump(arg))
            }
            other => Err(ParseError::new(
                start,
                format!("unknown sink `{other}`"),
            )),
        }
    }

    // ── source-kind parser ────────────────────────────────────────────────

    fn parse_source_kind(&mut self) -> Result<SourceKind> {
        let name = self.parse_ident()?;
        match name.as_str() {
            "usb"          => Ok(SourceKind::Usb),
            "lora"         => Ok(SourceKind::Lora),
            "programmatic" => Ok(SourceKind::Programmatic),
            other          => Err(ParseError::new(
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
            self.expect("=")?;
            let kind = self.parse_source_kind()?;
            self.expect(";")?;
            return Ok(Stmt::Source { name, kind });
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
                let sink = self.parse_sink()?;
                self.expect(";")?;
                sinks.push(sink);
            }
            return Ok(Stmt::Emit { sinks });
        }

        Err(ParseError::new(
            self.pos,
            format!(
                "expected `source`, `let`, or `emit`; found `{}`",
                &self.src[self.pos..].chars().take(16).collect::<String>()
            ),
        ))
    }

    // ── top-level ─────────────────────────────────────────────────────────

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
///
/// # Errors
/// Returns a [`ParseError`] with an offset and human-readable message if the
/// source text is syntactically invalid.
pub fn parse(src: &str) -> Result<Program> {
    Parser::new(src).parse_program()
}
