# RIVR — Architectuur

---

## Systeemdiagram

```
┌─────────────────────────────────────────────────────────────┐
│  ESP32 (RIVR embedded node)                                  │
│                                                              │
│  ┌──────────┐   ┌──────────────┐   ┌────────────────────┐  │
│  │ SX1262   │   │  Ring-buffer │   │   RIVR-pipeline    │  │
│  │ (LoRa)   │──▶│  rx_ring     │──▶│  rivr_inject_event │  │
│  └──────────┘   └──────────────┘   │        │           │  │
│       ▲                            │  DAG-knooppunten   │  │
│       │                            │  filter, budget,   │  │
│  ┌────┴──────┐                     │  throttle, window  │  │
│  │ rf_tx     │◀────────────────────│  rivr_emit_dispatch│  │
│  │ TX-wachtr.│                     └────────────────────┘  │
│  └───────────┘                                              │
│                                                              │
│  Hoofdlus (10 ms):                                           │
│    rivr_engine_run(clock_ms, clock_ticks)                   │
│    tx_drain_loop()                                           │
└─────────────────────────────────────────────────────────────┘
```

---

## Subsysteem-verantwoordelijkheden

### `rivr_core` (Rust, `no_std+alloc`)

| Module | Verantwoordelijkheid |
|---|---|
| `parser.rs` | Tokeniseer en parseer RIVR-broncode → AST |
| `compiler.rs` | Valideer AST, genereer opcode-bytes |
| `runtime/engine.rs` | Houd klokken bij, verdeel events naar knooppunten |
| `runtime/node.rs` | Voer elke operator uit (filter, window, budget, …) |
| `runtime/value.rs` | `Value`-enum, kloon en weergave |
| FFI (`ffi` feature) | `extern "C"` functies voor aanroep vanuit C-firmware |

### `rivr_host` (Rust, std)

| Module | Verantwoordelijkheid |
|---|---|
| `main.rs` | 8 standalone demo's (één per operator-categorie) |
| `replay.rs` | Replay 2.0: event-reeksen opnemen, afspelen en asserten (JSONL) |

### `firmware_core` (C)

| Bestand | Verantwoordelijkheid |
|---|---|
| `main.c` | `app_main`, initialisatie, SIM-frame-injectie, hoofdlus |
| `platform_esp32.c` | Klokdivisie, SPI-bus, GPIO-initialisatie |
| `radio_sx1262.c` | SX1262 op-/afzet, ring-buffer, TX-wachtrij |
| `protocol.c` | Binaire frame-encode/decode met CRC-16 |
| `routing.c` | Dedupe-cache, hop-limietcontrole, buurttabel |

### `rivr_layer` (C)

| Bestand | Verantwoordelijkheid |
|---|---|
| `rivr_embed.c` | `rivr_engine_init`, `rivr_inject_event`, `rivr_engine_run` |
| `rivr_sources.c` | Koppel hardware-events aan RIVR-bron-IDs |
| `rivr_sinks.c` | `rf_tx_sink_cb`, `usb_print_sink_cb`, `log_sink_cb` |
| `default_program.h` | Selecteerbare RIVR-programma-strings |

---

## Gegevensstroom — ontvangen LoRa-frame (end-to-end)

```
SX1262 RX-IRQ
   ↓
radio_sx1262_rx_isr()          schrijft bytes naar rx_ring_buf
   ↓
main_loop() / rx_drain_task()  leest frame uit ring-buffer
   ↓
protocol_decode()              valideert CRC-16, ontleedt koptekst
   ↓
routing_dedupe_check()         gooit duplicaten weg (IDs bijgehouden in wachtrijcache)
   ↓
routing_should_forward()       beslist of opnieuw uitzenden nodig is
   ↓
rivr_inject_event()            voegt event in RIVR-engine in
   ↓
DAG-doorloop                   filter → budget → throttle → … (synchroon)
   ↓
rivr_emit_dispatch()           roept geregistreerde sink-callback aan
   ↓
rf_tx_sink_cb()                plaatst frame in TX-wachtrij
   ↓
tx_drain_loop()                pop + radio_transmit()
   ↓
SX1262 TX
```

---

## Klokken en tijdsdomeinen

| Klok-ID | Naam | Eenheid | Bron |
|---|---|---|---|
| 0 | `mono` | milliseconden | `esp_timer_get_time() / 1000` |
| 1 | `lmp` | logische tikken | Lamport-klok — verhoogd bij elke verzending/ontvangst |

Tijdgebaseerde operatoren (`window.ms`, `throttle.ms`, `debounce.ms`) gebruiken
klok 0.  Tikgebaseerde operatoren (`window.ticks`, `throttle.ticks`,
`delay.ticks`) gebruiken klok 1 en zijn daarmee causaal in plaats van
horologisch.

---

## Geheugenmodel

| Segment | Inhoud | Grootte (ca.) |
|---|---|---|
| BSS | `ENGINE_SLOT: MaybeUninit<Engine>` | ~16 KB |
| BSS | `rx_ring_buf[2048]` | 2 KB |
| BSS | `tx_queue[RF_TX_QUEUE_CAP × frame_size]` | variabel |
| Flash | RIVR-programmastring (ingebakken in firmware) | < 1 KB |
| Heap | Geen gebruik na `app_main()` | 0 |

Na initialisatie doet de engine geen dynamische allocatie —
`alloc::vec!` wordt alleen gebruikt bij het initialiseren van de DAG.

---

## Feature-flags

| Flag | Effect |
|---|---|
| `std` *(standaard)* | Gebruik van Rust std ; voor host-demo's |
| `alloc` | `no_std + alloc` ; voor ingebedde doelen met allocator |
| `ffi` | Exporteer `extern "C"` symbolen voor gebruik vanuit C |
| `embedded` | Schakel `std`-aannames uit; activeer `alloc` + `ffi` |

---

## Bouw-artefacten

| Artefact | Pad | Gebruikt door |
|---|---|---|
| `librivr_core.a` | `rivr_core/target/<profiel>/` | `main/CMakeLists.txt` |
| `rivr_host` demo- binair | `rivr_core/target/debug/rivr_host` | desktop-tests |
| PlatformIO firmware | `.pio/build/esp32_sim/firmware.bin` | flashen naar ESP32 |
| ESP-IDF firmware | `build/rivr_node.bin` | flashen via `idf.py` |
