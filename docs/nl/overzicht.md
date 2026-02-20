# RIVR — Overzicht

RIVR is een minimalistische stream-verwerkingstaal en -runtime voor ingebedde
LoRa-meshnodes.  Schrijf declaratieve pijplijnen in een opgemaakte DSL; de
compiler zet ze om naar een toestandsloze DAG die zonder heap-allocatie draait
op microcontrollers.

---

## Welk probleem lost RIVR op?

| Praktisch probleem | RIVR-oplossing |
|---|---|
| LoRa verplicht ≤ 1% duty-cycle | `budget.toa_us` en `budget.airtime` operatoren op elke pijplijn |
| Embedded MCU heeft geen heap | Anonieme pijplijnen worden statisch opgeslagen in BSS (`ENGINE_SLOT`) |
| Geen RTOS-threads beschikbaar | Ééntraps poll-lus: `rivr_engine_run()` elke 10 ms |
| Binair LoRa-frameformaat | `filter.pkt_type(N)` inspecteert headeroffset 3 van ruwe bytes |
| Lamport-klok voor logisch tijdstip | Tweedeklokinject via `clock_id = 1` (`@lmp`-bronnen) |

---

## Repository-indeling

```
Rivr/
├── rivr_core/                # Rust — parser + compiler + runtime + FFI
│   └── src/
│       ├── lib.rs            # feature-gates & herexports
│       ├── ast.rs            # AST-typen (PipeOp enum, enz.)
│       ├── parser.rs         # handgeschreven recursive-descent parser
│       ├── compiler.rs       # AST → opcode-bytes
│       └── runtime/
│           ├── engine.rs     # Engine struct, injection, run
│           ├── node.rs       # uitvoering per operator/node
│           └── value.rs      # Value enum (Int, Bool, Str, Bytes, Window, …)
├── rivr_host/                # Rust (std) — desktop-demo's en Replay 2.0
│   └── src/
│       ├── main.rs           # 8 demo's: window, budget, debounce, pkt_type, …
│       └── replay.rs         # Replay 2.0 — record / replay / assert (JSONL)
├── firmware_core/            # C — ESP32 stuurprogramma's + protocol + routing
│   ├── main.c                # app_main, main-lus, SIM-frame-injectie
│   ├── platform_esp32.c/h    # klokken, SPI, GPIO-pinnen
│   ├── radio_sx1262.c/h      # SX1262 LoRa driver, TX-wachtrij, ring-buffer
│   ├── protocol.c/h          # binaire frame-encode/decode met CRC-16
│   └── routing.c/h           # dedupe-cache, hop-limiet, buurttabel
└── rivr_layer/               # C — lijmlaag RIVR↔firmware
    ├── rivr_embed.c/h        # engine_init, inject, run, emit-dispatch
    ├── rivr_sources.c/h      # bronregistratie (rf, usb, …)
    ├── rivr_sinks.c/h        # sink-callbacks (rf_tx, usb_print, log)
    └── default_program.h     # selecteerbare RIVR-programma's via #define
```

---

## Kernbegrippen

### Stamp

Elk event draagt een `(clock_id, timestamp)` stempel:

```
(0, 1 234 567)   → clock 0 (mono), 1 234,567 seconden na opstart
(1,          9)  → clock 1 (lmp),  Lamport-tik 9
```

### Value

Pijplijnen verwerken getypte waarden:

| Type | Gebruik |
|---|---|
| `Str` | UTF-8 chatberichten |
| `Bytes` | Ruwe LoRa-frames (binair protocol) |
| `Int` | Tellers, TTL-waarden |
| `Bool` | Filtervlaggen |
| `Window` | Gebufferde verzameling events (na `window.*`) |
| `Unit` | Leeg signaal |

### Pijplijn (`|>`)

```rivr
let chat = rf_rx
  |> filter.pkt_type(1)
  |> budget.toa_us(360000, 0.10, 360000)
  |> throttle.ticks(1);
```

Elke `|>`-operatie wordt één knoop in de DAG.
Geen threads, geen kanalen — elke `rivr_inject_event()` doorloopt de keten synchroon.

### Emit

```rivr
emit {
  rf_tx(chat);
  usb_print(chat);
}
```

`emit`-clausules verbinden pijplijnen met C-sink-callbacks.

---

## Componenten in één oogopslag

| Onderdeel | Taal | Rol |
|---|---|---|
| `rivr_core` | Rust (`no_std+alloc`) | Parser, compiler, runtime, FFI-exports |
| `rivr_host` | Rust (std) | Desktop-demo's, Replay 2.0 |
| `firmware_core` | C | ESP32 drivers, protocol, routing |
| `rivr_layer` | C | Lijmlaag: bronnen, sinks, embedAPI |

---

## Snel aan de slag

### Desktop-demo's uitvoeren

```powershell
cd e:\Projects\Rivr\rivr_core
cargo run -p rivr_host
```

### Firmware bouwen (simulatiemodus)

```powershell
# Stap 1: bouw de Rust-bibliotheek
cd e:\Projects\Rivr\rivr_core
cargo build --features ffi

# Stap 2: bouw en flash via PlatformIO
cd ..
pio run -e esp32_sim -t upload
pio device monitor --baud 115200
```

Zie [bouwhandleiding.md](bouwhandleiding.md) voor een volledige installatie op hardware.
