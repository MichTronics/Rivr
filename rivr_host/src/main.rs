mod replay;

use rivr_core::{compile, parse, Engine, Event, Stamp, Value};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

fn run(src: &str) -> Engine {
    let program = match parse(src) {
        Ok(p)  => p,
        Err(e) => { eprintln!("parse error: {e}"); std::process::exit(1); }
    };
    let (engine, warns) = match compile(&program) {
        Ok(pair) => pair,
        Err(errs) => {
            for e in errs { eprintln!("{e}"); }
            std::process::exit(1);
        }
    };
    for w in warns { eprintln!("{w}"); }
    engine
}

fn inject_str(engine: &mut Engine, src: &str, tick: u64, text: &str) {
    let ev = Event { stamp: Stamp::mono(tick), v: Value::Str(text.into()), tag: None, seq: 0 };
    let _ = engine.inject(src, ev);
}

fn inject_tagged(engine: &mut Engine, src: &str, tick: u64, text: &str, tag: &str) {
    let ev = Event {
        stamp: Stamp::mono(tick),
        v:     Value::Str(text.into()),
        tag:   Some(tag.into()),
        seq:   0,
    };
    let _ = engine.inject(src, ev);
}

fn inject_lmp(engine: &mut Engine, src: &str, tick: u64, text: &str) {
    let ev = Event { stamp: Stamp::at(1, tick), v: Value::Str(text.into()), tag: None, seq: 0 };
    let _ = engine.inject(src, ev);
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 1 – window.ms: sliding time window (ms-domain)
// ─────────────────────────────────────────────────────────────────────────────

fn demo1() {
    println!("\n═══ Demo 1 – window.ms(500) ════════════════════════════════\n");
    let rivr = r#"
source usb = usb;
let words = usb
  |> window.ms(500);
emit { io.usb.print(words); }
"#;
    let mut eng = run(rivr);
    eng.print_graph();
    for (tick, word) in [
        (100u64, "hello"),
        (200,    "world"),
        (300,    "foo"),
        (700,    "bar"),      // past 500 ms boundary
        (800,    "baz"),
    ] { inject_str(&mut eng, "usb", tick, word); }
    eng.run(500);
    eng.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 2 – map.upper + filter.nonempty
// ─────────────────────────────────────────────────────────────────────────────

fn demo2() {
    println!("\n═══ Demo 2 – map.upper + filter.nonempty ═══════════════════\n");
    let rivr = r#"
source usb = usb;
let upper = usb
  |> map.upper()
  |> filter.nonempty();
emit { io.usb.print(upper); }
"#;
    let mut eng = run(rivr);
    eng.print_graph();
    for (tick, word) in [(0u64,""), (100,"hello"), (200,""), (300,"world")] {
        inject_str(&mut eng, "usb", tick, word);
    }
    eng.run(200);
    eng.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 3 – fold.count + throttle.ms
// ─────────────────────────────────────────────────────────────────────────────

fn demo3() {
    println!("\n═══ Demo 3 – fold.count + throttle.ms(200) ═════════════════\n");
    let rivr = r#"
source evt = usb;
let count = evt
  |> fold.count()
  |> throttle.ms(200);
emit { io.usb.print(count); }
"#;
    let mut eng = run(rivr);
    eng.print_graph();
    for (tick, word) in [
        (0u64,"a"), (50,"b"), (100,"c"),
        (250,"d"), (300,"e"), (350,"f"),
    ] { inject_str(&mut eng, "evt", tick, word); }
    eng.run(300);
    eng.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 4 – logical clock (LMP) + delay.ticks + window.ticks
// ─────────────────────────────────────────────────────────────────────────────

fn demo4() {
    println!("\n═══ Demo 4 – LMP logical clock: delay.ticks(2) + window.ticks(5) ═══\n");
    // source annotated with @lmp – events carry Stamp { clock:1, tick:N }
    let rivr = r#"
source rf @lmp = rf;
let buffered = rf
  |> delay.ticks(2)
  |> window.ticks(5);
emit { io.usb.print(buffered); }
"#;
    let mut eng = run(rivr);
    eng.print_graph();
    // Inject 9 LMP-domain events (logical ticks 1..=9)
    let frames = [
        "BEACON:0x0A", "BEACON:0x0B", "DATA:hello",
        "DATA:world",  "ACK:0x01",    "DATA:foo",
        "BEACON:0x0C", "DATA:bar",    "ACK:0x02",
    ];
    for (i, frame) in frames.iter().enumerate() {
        inject_lmp(&mut eng, "rf", i as u64 + 1, frame);
    }
    eng.run(500);
    eng.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 5 – budget.airtime + filter.kind + throttle.ticks
// ─────────────────────────────────────────────────────────────────────────────

fn demo5() {
    println!("\n═══ Demo 5 – budget.airtime(360000, 0.10) ════════════════════\n");
    // 360 000 ticks window, 10 % duty → 36 000 budget ticks.
    // filter.kind("CHAT") keeps only events whose tag = "CHAT".
    // throttle.ticks(1) de-duplicates within same tick.
    let rivr = r#"
source rf = rf;
let tx_ok = rf
  |> budget.airtime(360000, 0.10)
  |> filter.kind("CHAT")
  |> throttle.ticks(1);
emit { io.lora.tx(tx_ok); }
"#;
    let mut eng = run(rivr);
    eng.print_graph();
    // 5 CHAT messages and 3 SENSOR messages
    let msgs: &[(u64, &str, &str)] = &[
        (100,  "CHAT:hello",  "CHAT"),
        (200,  "SENSOR:23.1", "SENSOR"),
        (300,  "CHAT:world",  "CHAT"),
        (400,  "SENSOR:23.4", "SENSOR"),
        (500,  "CHAT:rivr",   "CHAT"),
        (600,  "SENSOR:23.7", "SENSOR"),
        (700,  "CHAT:rocks",  "CHAT"),
        (800,  "CHAT:end",    "CHAT"),
    ];
    for &(tick, text, tag) in msgs {
        inject_tagged(&mut eng, "rf", tick, text, tag);
    }
    eng.run(500);
    eng.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 6 – multi-source merge
// ─────────────────────────────────────────────────────────────────────────────

fn demo6() {
    println!("\n═══ Demo 6 – merge(usb, lora) + map.upper ═══════════════════\n");
    let rivr = r#"
source usb = usb;
source lora = lora;
let both = merge(usb, lora)
  |> map.upper()
  |> filter.nonempty();
emit { io.usb.print(both); }
"#;
    let mut eng = run(rivr);
    eng.print_graph();
    inject_str(&mut eng, "usb",  100, "from-usb");
    inject_str(&mut eng, "lora", 150, "from-lora");
    inject_str(&mut eng, "usb",  200, "usb-again");
    eng.run(300);
    eng.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 7 – window.ticks with cap: FlushEarly vs DropOldest
// ─────────────────────────────────────────────────────────────────────────────

fn demo7() {
    println!("\n═══ Demo 7 – window.ticks(10, 3, policy): FlushEarly vs DropOldest ═══\n");

    // ── Version A: FlushEarly ────────────────────────────────────────────────
    // When the buffer hits cap=3, emit immediately and start a fresh window.
    let flush_early_rivr = r#"
source rf @lmp = rf;
let windowed = rf
  |> window.ticks(10, 3, "flush_early");
emit { io.usb.print(windowed); }
"#;

    println!("── A) flush_early(cap=3): 7 events in ticks 1..7 ──\n");
    let mut eng_a = run(flush_early_rivr);
    eng_a.print_graph();
    for i in 1u64..=7 {
        inject_lmp(&mut eng_a, "rf", i, &format!("pkt#{i}"));
    }
    eng_a.run(500);
    eng_a.print_stats();

    // ── Version B: DropOldest ────────────────────────────────────────────────
    // When the buffer hits cap=3, evict the oldest entry and keep accepting.
    let drop_oldest_rivr = r#"
source rf @lmp = rf;
let windowed = rf
  |> window.ticks(10, 3, "drop_oldest");
emit { io.usb.print(windowed); }
"#;

    println!("── B) drop_oldest(cap=3): same 7 events ──\n");
    let mut eng_b = run(drop_oldest_rivr);
    eng_b.print_graph();
    for i in 1u64..=7 {
        inject_lmp(&mut eng_b, "rf", i, &format!("pkt#{i}"));
    }
    // flush the tick-10 window boundary
    inject_lmp(&mut eng_b, "rf", 12, "tick-flush");
    eng_b.run(500);
    eng_b.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo 8 – budget.toa_us (Time-on-Air radio duty-cycle limiter)
// ─────────────────────────────────────────────────────────────────────────────

fn demo8() {
    println!("\n═══ Demo 8 – budget.toa_us(360000, 0.10, 400) ═══\n");
    // window_ms = 360_000 ms (1 hour)
    // duty      = 0.10  → budget_us = 36_000_000 µs per hour
    // toa_us    = 400   → each LoRa packet costs 400 µs on-air time
    // max safe packets per hour = 90 000
    let rivr = r#"
source rf @lmp = rf;
let safe_tx = rf
  |> budget.toa_us(360000, 0.10, 400);
emit { io.lora.tx(safe_tx); }
"#;
    let mut eng = run(rivr);
    eng.print_graph();

    println!("Injecting 10 packets (each 400 µs ToA):");
    for i in 1u64..=10 {
        inject_lmp(&mut eng, "rf", i * 100, &format!("LORA_PKT_{i}"));
    }
    eng.run(500);
    eng.print_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay 2.0 – record + assert
// ─────────────────────────────────────────────────────────────────────────────

fn do_replay_assert() {
    println!("\n═══ Replay 2.0 – record then assert ═══════════════════════\n");
    let rivr = r#"
source usb = usb;
let words = usb
  |> window.ms(500);
emit { io.usb.print(words); }
"#;
    // Phase 1: record
    let mut eng1 = run(rivr);
    let events: Vec<(&str, Event)> = vec![
        ("usb", Event { stamp: Stamp::mono(100), v: Value::Str("hello".into()), tag: None, seq: 0 }),
        ("usb", Event { stamp: Stamp::mono(200), v: Value::Str("world".into()), tag: None, seq: 0 }),
        ("usb", Event { stamp: Stamp::mono(300), v: Value::Str("foo".into()),   tag: None, seq: 0 }),
        ("usb", Event { stamp: Stamp::mono(700), v: Value::Str("bar".into()),   tag: None, seq: 0 }),
    ];
    let trace = replay::record(&mut eng1, &events);
    println!("── recorded trace ──\n{trace}\n");

    // Phase 2: replay + assert
    let mut eng2 = run(rivr);
    match replay::replay_and_assert(&mut eng2, &trace) {
        Ok(())   => println!("Replay assertion: PASSED ✓\n"),
        Err(diffs) => println!("Replay assertion: FAILED ({} diffs) ✗\n", diffs.len()),
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay (legacy --replay flag)
// ─────────────────────────────────────────────────────────────────────────────

fn do_replay(path: &str) {
    let rivr = r#"
source usb = usb;
let words = usb
  |> window.ms(500);
emit { io.usb.print(words); }
"#;
    let mut eng = run(rivr);
    let content = std::fs::read_to_string(path)
        .unwrap_or_else(|_| {
            // Built-in example log if no file given (new JSONL format)
            concat!(
                "{\"kind\":\"in\",\"channel\":\"usb\",\"stamp\":{\"clock\":0,\"tick\":100},\"v\":\"hello\"}\n",
                "{\"kind\":\"in\",\"channel\":\"usb\",\"stamp\":{\"clock\":0,\"tick\":200},\"v\":\"world\"}\n",
                "{\"kind\":\"in\",\"channel\":\"usb\",\"stamp\":{\"clock\":0,\"tick\":300},\"v\":\"foo\"}\n",
                "{\"kind\":\"in\",\"channel\":\"usb\",\"stamp\":{\"clock\":0,\"tick\":700},\"v\":\"bar\"}\n",
            ).into()
        });
    replay::replay_legacy(&mut eng, &content);
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

fn main() {
    let args: Vec<String> = std::env::args().collect();

    if args.get(1).map(|s| s.as_str()) == Some("--replay") {
        let path = args.get(2).map(|s| s.as_str()).unwrap_or("demo1.jsonl");
        do_replay(path);
        return;
    }

    demo1();
    demo2();
    demo3();
    demo4();
    demo5();
    demo6();
    demo7();
    demo8();
    do_replay_assert();

    println!("\n✓ all demos complete\n");
}
