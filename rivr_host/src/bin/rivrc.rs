//! # rivrc — RIVR source compiler CLI
//!
//! Parses and compiles a `.rivr` program, then prints (or exports) the
//! resulting node graph.
//!
//! ```text
//! USAGE:
//!   rivrc [FLAGS] <program.rivr>
//!
//!   <program.rivr>   Path to the RIVR source file to compile.
//!
//! FLAGS:
//!   --check          Exit 0/1 without printing anything (CI/lint mode).
//!   --dot            Output GraphViz DOT to stdout instead of the table.
//!   --json           Output node graph as JSON to stdout.
//!   --version        Print version and exit.
//!   -h, --help       Print this help and exit.
//! ```
//!
//! Exit codes:
//!   0 — success
//!   1 — parse, compile, or I/O error
//!   2 — usage error

use std::{env, fmt::Write as FmtWrite, fs, process};

use rivr_core::{compile, parse, runtime::node::NodeKind};

const VERSION: &str = env!("CARGO_PKG_VERSION");

fn main() {
    let args: Vec<String> = env::args().collect();

    // ── argument parsing ────────────────────────────────────────────────
    let mut path_arg: Option<&str> = None;
    let mut check_only = false;
    let mut emit_dot = false;
    let mut emit_json = false;

    for arg in args.iter().skip(1) {
        match arg.as_str() {
            "--check" | "-c" => check_only = true,
            "--dot" => emit_dot = true,
            "--json" => emit_json = true,
            "--version" | "-V" => {
                println!("rivrc {VERSION}");
                process::exit(0);
            }
            "--help" | "-h" => {
                usage();
                process::exit(0);
            }
            _ if arg.starts_with('-') => {
                eprintln!("rivrc: unknown flag '{arg}'");
                usage();
                process::exit(2);
            }
            _ => {
                if path_arg.is_some() {
                    eprintln!("rivrc: too many positional arguments");
                    usage();
                    process::exit(2);
                }
                path_arg = Some(arg.as_str());
            }
        }
    }

    let path = match path_arg {
        Some(p) => p,
        None => {
            eprintln!("rivrc: no input file specified");
            usage();
            process::exit(2);
        }
    };

    // ── read source ─────────────────────────────────────────────────────
    let src = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("rivrc: cannot read '{path}': {e}");
            process::exit(1);
        }
    };

    // ── parse ────────────────────────────────────────────────────────────
    let program = match parse(&src) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("rivrc: parse error in '{path}':");
            eprintln!("  {e}");
            process::exit(1);
        }
    };

    // ── compile ──────────────────────────────────────────────────────────
    let (engine, warnings) = match compile(&program) {
        Ok((eng, warns)) => (eng, warns),
        Err(errors) => {
            eprintln!("rivrc: compile error(s) in '{path}':");
            for e in &errors {
                eprintln!("  {e}");
            }
            process::exit(1);
        }
    };
    for w in &warnings {
        eprintln!("rivrc: {path}: {w}");
    }

    if check_only {
        process::exit(0);
    }

    if emit_dot {
        print_dot(path, &engine.nodes);
        return;
    }

    if emit_json {
        print_json(path, &engine.nodes);
        return;
    }

    // ── default: human-readable table ────────────────────────────────────
    println!("RIVR '{path}'  — {n} node(s)", n = engine.nodes.len());
    println!("{}", "─".repeat(72));
    println!("{:<4}  {:<24}  KIND / PARAMS", "ID", "NAME");
    println!("{}", "─".repeat(72));

    for (id, node) in engine.nodes.iter().enumerate() {
        let detail = node_detail(&node.kind);
        println!("{id:<4}  {name:<24}  {detail}", name = node.name);
    }

    println!("{}", "─".repeat(72));
    print_edges(&engine.nodes);
}

// ── helpers ──────────────────────────────────────────────────────────────────

fn usage() {
    eprintln!("RIVR compiler  (rivrc {VERSION})");
    eprintln!();
    eprintln!("USAGE:  rivrc [FLAGS] <program.rivr>");
    eprintln!();
    eprintln!("FLAGS:");
    eprintln!("  --check           CI mode: exit 0 on success, 1 on error, no output");
    eprintln!("  --dot             Emit GraphViz DOT to stdout");
    eprintln!("  --json            Emit node graph as JSON to stdout");
    eprintln!("  --version, -V     Print version");
    eprintln!("  --help, -h        Print this help");
}

/// Produce a short human-readable description of a node's kind and key params.
fn node_detail(kind: &NodeKind) -> String {
    match kind {
        NodeKind::Source { name, clock, interval_ms } => {
            let mut s = format!("Source(name={name:?}, clock={clock}");
            if let Some(ms) = interval_ms {
                let _ = write!(s, ", timer={ms}ms");
            }
            s.push(')');
            s
        }
        NodeKind::Emit { sink }  => format!("Emit(sink={sink:?})"),
        NodeKind::Merge            => "Merge".into(),
        NodeKind::Tag { label }    => format!("Tag({label:?})"),
        NodeKind::MapUpper => "MapUpper".into(),
        NodeKind::MapLower => "MapLower".into(),
        NodeKind::MapTrim  => "MapTrim".into(),
        NodeKind::FilterNonempty => "FilterNonempty".into(),
        NodeKind::FilterKind { kind } => format!("FilterKind({kind:?})"),
        NodeKind::FilterPktType { pkt_type } => format!("FilterPktType({pkt_type})"),
        NodeKind::FoldCount { count } => format!("FoldCount({count})"),
        NodeKind::FoldSum { sum }     => format!("FoldSum(acc={sum})"),
        NodeKind::FoldLast { last }   => format!("FoldLast(last={last:?})"),
        NodeKind::WindowTicks { duration, cap, .. } =>
            format!("WindowTicks(dur={duration}, cap={cap})"),
        NodeKind::WindowMs { inner } => format!("WindowMs(inner={inner:?})"),
        NodeKind::ThrottleTicks { interval, .. } => format!("ThrottleTicks({interval})"),
        NodeKind::ThrottleMs { inner } => format!("ThrottleMs(inner={inner:?})"),
        NodeKind::DelayTicks { delay, .. } => format!("DelayTicks({delay})"),
        NodeKind::DebounceMs { inner } => format!("DebounceMs(inner={inner:?})"),
        NodeKind::Budget { rate_per_tick, burst, .. } =>
            format!("Budget(rate={rate_per_tick}/tick, burst={burst})"),
        NodeKind::AirtimeBudget { window_ticks, budget_per_window, .. } =>
            format!("AirtimeBudget(win={window_ticks}ticks, budget={budget_per_window})"),
        NodeKind::ToaBudget { window_ms, budget_us, toa_us_per_event, .. } =>
            format!("ToaBudget(win={window_ms}ms, budget={budget_us}\u{b5}s, toa={toa_us_per_event}\u{b5}s)"),
    }
}

/// Short label for DOT / JSON node categories.
fn node_category(kind: &NodeKind) -> &'static str {
    match kind {
        NodeKind::Source { .. } => "source",
        NodeKind::Emit { .. } => "emit",
        NodeKind::Merge => "merge",
        NodeKind::Tag { .. } => "tag",
        NodeKind::MapUpper | NodeKind::MapLower | NodeKind::MapTrim => "map",
        NodeKind::FilterNonempty | NodeKind::FilterKind { .. } | NodeKind::FilterPktType { .. } => {
            "filter"
        }
        NodeKind::FoldCount { .. } | NodeKind::FoldSum { .. } | NodeKind::FoldLast { .. } => "fold",
        NodeKind::WindowTicks { .. } | NodeKind::WindowMs { .. } => "window",
        NodeKind::ThrottleTicks { .. } | NodeKind::ThrottleMs { .. } => "throttle",
        NodeKind::DelayTicks { .. } | NodeKind::DebounceMs { .. } => "delay",
        NodeKind::Budget { .. } | NodeKind::AirtimeBudget { .. } | NodeKind::ToaBudget { .. } => {
            "budget"
        }
    }
}

/// Escape a string for use in a GraphViz label.
fn dot_escape(s: &str) -> String {
    s.replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
}

/// Output GraphViz DOT format.
fn print_dot(path: &str, nodes: &[rivr_core::runtime::node::Node]) {
    println!("// RIVR stream graph — {path}");
    println!("digraph rivr {{");
    println!("  rankdir=LR;");
    println!("  node [shape=box style=filled fontname=\"monospace\" fontsize=10];");

    // Node colours by category.
    for (id, node) in nodes.iter().enumerate() {
        let cat = node_category(&node.kind);
        let label = dot_escape(&format!("{}\n{}", node.name, node_detail(&node.kind)));
        let color = match cat {
            "source" => "#cce5ff",
            "emit" => "#d4edda",
            "filter" => "#fff3cd",
            "budget" => "#f8d7da",
            "tag" => "#e2d9f3",
            _ => "#f8f9fa",
        };
        println!("  {id} [label=\"{label}\" fillcolor=\"{color}\"];");
    }

    // Edges.
    println!();
    for (id, node) in nodes.iter().enumerate() {
        for &out in &node.outputs {
            println!("  {id} -> {out};");
        }
    }

    println!("}}");
}

/// Output a compact JSON representation of the node graph.
fn print_json(path: &str, nodes: &[rivr_core::runtime::node::Node]) {
    println!("{{");
    println!("  \"program\": \"{}\",", dot_escape(path));
    println!("  \"node_count\": {},", nodes.len());
    println!("  \"nodes\": [");
    for (id, node) in nodes.iter().enumerate() {
        let comma = if id + 1 < nodes.len() { "," } else { "" };
        let outputs: Vec<String> = node.outputs.iter().map(|o| o.to_string()).collect();
        let inputs: Vec<String> = node.inputs.iter().map(|i| i.to_string()).collect();
        println!(
            "    {{\"id\":{id},\"name\":\"{nm}\",\"category\":\"{cat}\",\
\"detail\":\"{det}\",\"inputs\":[{ins}],\"outputs\":[{outs}]}}{comma}",
            nm = dot_escape(&node.name),
            cat = node_category(&node.kind),
            det = dot_escape(&node_detail(&node.kind)),
            ins = inputs.join(","),
            outs = outputs.join(","),
        );
    }
    println!("  ]");
    println!("}}");
}

/// Print the edge list (nodes with their upstream inputs).
fn print_edges(nodes: &[rivr_core::runtime::node::Node]) {
    let has_edges = nodes.iter().any(|n| !n.inputs.is_empty());
    if !has_edges {
        return;
    }
    println!("\nEdges:");
    for (id, node) in nodes.iter().enumerate() {
        if !node.inputs.is_empty() {
            let ins: Vec<String> = node.inputs.iter().map(|i| i.to_string()).collect();
            println!("  {id} ({}) ← [{}]", node.name, ins.join(", "));
        }
    }
}
