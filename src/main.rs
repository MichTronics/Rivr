//! # RIVR – Embedded-First Stream Language
//!
//! Entry point for the RIVR interpreter.  Demonstrates the full pipeline:
//!
//! ```text
//! RIVR source text
//!       │
//!       ▼  parser::parse()
//!      AST
//!       │
//!       ▼  compiler::compile()
//!   Engine (stream graph / DAG)
//!       │
//!       ▼  engine.inject() + engine.run_to_idle()
//!   Output events
//! ```
//!
//! ## Demo programs
//! Three demo scripts are embedded here to exercise the major features:
//!
//! - **Demo 1** – the canonical example from the spec (map/filter/window).
//! - **Demo 2** – rate-limiting with `throttle` and `budget`.
//! - **Demo 3** – stream merging, `fold.count`, and trace tagging.
//!
//! ## Replay
//! After Demo 1 runs, all injected source events are written to
//! `rivr_replay.jsonl`.  A replay run re-reads the file and should produce
//! exactly the same output.

mod ast;
mod parser;
mod compiler;
mod runtime;

use runtime::{Event, Value};
use compiler::compile;
use parser::parse;

// ─────────────────────────────────────────────────────────────────────────────
// Demo RIVR programs
// ─────────────────────────────────────────────────────────────────────────────

/// Canonical example from the spec: USB → upper → filter → window → print.
const DEMO_1: &str = r#"
// Demo 1: USB text pipeline
// source receives raw lines, we upper-case, filter empties, collect
// into 2-second windows, then print.
source usb = usb;
let lines = usb
  |> map.upper()
  |> filter.nonempty()
  |> window.ms(2000);
emit {
  io.usb.print(lines);
}
"#;

/// Rate-limiting demo: throttle + budget.
const DEMO_2: &str = r#"
// Demo 2: Rate limiting
// Sensor fires rapidly; throttle to max 1 event/500ms, then apply
// a token-bucket budget of 2 events/s with burst 3.
source sensor = usb;
let limited = sensor
  |> throttle.ms(500)
  |> budget(2.0, 3)
  |> tag("rate-limited");
emit {
  io.usb.print(limited);
}
"#;

/// Merge + count + tag demo.
const DEMO_3: &str = r#"
// Demo 3: Merge two sources and count events
source usb  = usb;
source lora = lora;
let combined = merge(usb, lora)
  |> filter.nonempty()
  |> fold.count()
  |> tag("count");
emit {
  io.usb.print(combined);
  io.debug.dump(combined);
}
"#;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

fn banner(title: &str) {
    println!();
    println!("╔═══════════════════════════════════════════════╗");
    println!("║  {:<45}║", title);
    println!("╚═══════════════════════════════════════════════╝");
}

fn section(title: &str) {
    println!("\n── {title} ─────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo runner
// ─────────────────────────────────────────────────────────────────────────────

/// Parse and compile `src`, then execute `run_fn` against the resulting engine.
fn run_demo(
    label: &str,
    src: &str,
    debug: bool,
    mut run_fn: impl FnMut(&mut runtime::Engine),
) {
    banner(label);

    // ── Parse ──────────────────────────────────────────────────────────────
    section("Parsing");
    let program = match parse(src) {
        Ok(p)  => { println!("  OK – {} statement(s)", p.stmts.len()); p }
        Err(e) => { eprintln!("  PARSE ERROR: {e}"); return; }
    };

    // ── Compile ────────────────────────────────────────────────────────────
    section("Compiling");
    let (mut engine, warnings) = match compile(&program) {
        Ok(r)  => r,
        Err(errs) => {
            for e in &errs { eprintln!("  {e}"); }
            return;
        }
    };
    println!("  OK – {} node(s) in graph", engine.nodes.len());
    for w in &warnings {
        println!("  {w}");
    }
    engine.debug = debug;

    // ── Print graph ────────────────────────────────────────────────────────
    if debug {
        engine.print_graph();
    }

    // ── Execute ────────────────────────────────────────────────────────────
    section("Execution");
    run_fn(&mut engine);

    // Flush any buffered window / debounce state.
    engine.flush_all();

    // ── Stats ──────────────────────────────────────────────────────────────
    if debug {
        engine.print_stats();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 1 – canonical USB pipeline
// ─────────────────────────────────────────────────────────────────────────────

fn demo_1() {
    run_demo("Demo 1 – USB text pipeline", DEMO_1, /*debug=*/ true, |engine| {
        // Attach a replay log.
        if let Ok(log) = runtime::ReplayLog::open_write("rivr_replay.jsonl") {
            engine.replay_log = Some(log);
        }

        // Synthetic USB events arriving over 5 virtual seconds.
        let events: Vec<(u64, &str)> = vec![
            (100,  "hello world"),
            (200,  ""),              // empty – should be filtered
            (300,  "  "),            // whitespace-only – should be filtered
            (500,  "rivr is alive"),
            (1000, "packet one"),
            (1500, "packet two"),
            (2100, "window two A"),  // crosses the 2000ms window boundary
            (2200, "window two B"),
            (3000, "final message"),
        ];

        for (t_ms, text) in events {
            let ev = Event::new(t_ms, Value::Str(text.to_string()));
            engine.inject("usb", ev).unwrap();
            engine.run_to_idle();
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 1 replay – re-run from the log file
// ─────────────────────────────────────────────────────────────────────────────

fn demo_1_replay() {
    banner("Demo 1 – Replay from rivr_replay.jsonl");

    let program = parse(DEMO_1).expect("parse");
    let (mut engine, _) = compile(&program).expect("compile");

    // Read back every recorded entry and inject it.
    match runtime::ReplayReader::open("rivr_replay.jsonl") {
        Err(e) => {
            println!("  (replay log not found: {e})");
            return;
        }
        Ok(reader) => {
            section("Replaying events");
            let mut count = 0;
            for entry_result in reader {
                match entry_result {
                    Err(e) => { eprintln!("  replay read error: {e}"); break; }
                    Ok(entry) => {
                        let ev = Event::new(entry.t_ms, entry.v);
                        engine.inject(&entry.source, ev).unwrap();
                        engine.run_to_idle();
                        count += 1;
                    }
                }
            }
            println!("  Replayed {count} source event(s)");
        }
    }

    engine.flush_all();
    section("Replay stats");
    engine.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 2 – rate-limiting
// ─────────────────────────────────────────────────────────────────────────────

fn demo_2() {
    run_demo("Demo 2 – Rate limiting (throttle + budget)", DEMO_2, /*debug=*/ false, |engine| {
        // Fire 10 events very rapidly (every 50ms), then a few slower ones.
        for i in 0..10u64 {
            let ev = Event::new(i * 50, Value::Str(format!("burst-{i}")));
            engine.inject("sensor", ev).unwrap();
            engine.run_to_idle();
        }
        // Slower events after 3 seconds.
        for i in 0..5u64 {
            let ev = Event::new(3000 + i * 600, Value::Str(format!("slow-{i}")));
            engine.inject("sensor", ev).unwrap();
            engine.run_to_idle();
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 3 – merge + count + tag
// ─────────────────────────────────────────────────────────────────────────────

fn demo_3() {
    run_demo("Demo 3 – Merge, fold.count, tag", DEMO_3, /*debug=*/ false, |engine| {
        // Interleaved USB and LoRa events.
        let usb_events = vec![
            (100u64, "alpha"),
            (300,    "beta"),
            (500,    ""),     // empty – filtered
            (700,    "gamma"),
        ];
        let lora_events = vec![
            (150u64, "L1"),
            (450,    "L2"),
            (650,    "L3"),
        ];

        // Merge by interleaving in timestamp order.
        let mut all: Vec<(u64, &str, &str)> = usb_events
            .iter()
            .map(|&(t, v)| (t, "usb", v))
            .chain(lora_events.iter().map(|&(t, v)| (t, "lora", v)))
            .collect();
        all.sort_by_key(|&(t, _, _)| t);

        for (t_ms, src, text) in all {
            let ev = Event::new(t_ms, Value::Str(text.to_string()));
            engine.inject(src, ev).unwrap();
            engine.run_to_idle();
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

fn main() {
    println!("╔═══════════════════════════════════════════════╗");
    println!("║       R I V R   Language Runtime  v0.1        ║");
    println!("║  Embedded-first · stream-oriented · replayable║");
    println!("╚═══════════════════════════════════════════════╝");

    // Check for replay mode: `rivr --replay`
    let args: Vec<String> = std::env::args().collect();
    let replay_mode = args.iter().any(|a| a == "--replay");

    if replay_mode {
        demo_1_replay();
    } else {
        demo_1();
        demo_2();
        demo_3();
    }

    println!();
    println!("All demos complete.  Run with --replay to replay Demo 1 from disk.");
}
