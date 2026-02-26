# RIVR — Build Guide

## Prerequisites

| Tool | Minimum version | Install |
|---|---|---|
| Rust | 1.75+ | `rustup update stable` |
| ESP-IDF | 5.1+ | https://docs.espressif.com/projects/esp-idf |
| espup | latest | `cargo install espup` (for Xtensa cross-compile) |
| PlatformIO | 6.x *(optional)* | `pip install platformio` |

---

## Step 1 — Build the Rust static library

The Rust library **must** be built before the C firmware.

### Debug (development)

```powershell
cd e:\Projects\Rivr\rivr_core
cargo build --features ffi
# Output:  target\debug\librivr_core.a
```

### Release (for flashing)

```powershell
cargo build --features ffi --release
# Output:  target\release\librivr_core.a
```

### Cross-compile for ESP32 (Xtensa LX6)

```powershell
# Install the Xtensa Rust toolchain (once):
espup install

# Cross-compile:
cargo +esp build --target xtensa-esp32-espidf --features ffi --release
# Output:  target\xtensa-esp32-espidf\release\librivr_core.a
```

> `main/CMakeLists.txt` automatically finds the library — it searches
> `target\release\` first, then `target\debug\`. No manual copy needed.

### Host demos (no hardware)

```powershell
cargo run -p rivr_host
```

Runs 8 demos covering every operator category plus Replay 2.0.

### `rivrc` — RIVR source compiler CLI

```powershell
# Print node graph
cargo run -p rivr_host --bin rivrc -- my_program.rivr

# CI / pre-commit check (exit 0 = ok, 1 = error, no output)
cargo run -p rivr_host --bin rivrc -- --check my_program.rivr

# Or after installing:
cargo install --path rivr_host --bin rivrc
rivrc my_program.rivr
```

Example output:
```
RIVR 'beacon.rivr'  — 4 node(s)
────────────────────────────────────────────────────────────────────────
ID    NAME                      KIND / PARAMS
────────────────────────────────────────────────────────────────────────
0     rf_rx                     Source(name="rf_rx", clock=1)
1     beacon_tick               Source(name="beacon_tick", clock=0, timer=30000ms)
2     chat                      ThrottleTicks(1)
3     emit_0                    Emit(sink=LoraTx)
```

---

## Step 2a — Build via PlatformIO *(recommended)*

### Build environments

| Environment | Role | Key flags |
|---|---|---|
| `esp32_sim` | Simulation — no SX1262 required | `RIVR_SIM_MODE=1`, `RIVR_SIM_TX_PRINT=1` |
| `esp32_hw` | Standard hardware node | `FEATURE_DISPLAY=1` |
| `repeater_esp32devkit_e22_900` | Dedicated relay with Rivr Fabric | `RIVR_FABRIC_REPEATER=1`, `RIVR_BUILD_REPEATER=1`, `FEATURE_DISPLAY=1` |
| `client_esp32devkit_e22_900` | Chat/data receiver; no relay | `RIVR_ROLE_CLIENT=1`, `FEATURE_DISPLAY=1` |

The repeater and client environments include a board-specific variant header via `-include`:  
`variants/esp32devkit_e22_900_repeater/config.h` and  
`variants/esp32devkit_e22_900_client/config.h` respectively.

### Simulation mode (no SX1262 hardware required)

```powershell
cd e:\Projects\Rivr

# Build simulation firmware
pio run -e esp32_sim

# Flash + open serial monitor
pio run  -e esp32_sim -t upload
pio device monitor -e esp32_sim --baud 115200
```

**Expected UART output:**

```
I (...) MAIN: ═══ RIVR Embedded Node booting ═══
I (...) MAIN: *** SIMULATION MODE: no real SX1262 hardware ***
I (...) RADIO: radio_init_buffers_only: ringbufs ready (SIM MODE)
I (...) RIVR_SINK: sinks registered: rf_tx, usb_print, log
I (...) RIVR_EMBED: engine ready
I (...) MAIN: [SIM] injecting 5 PKT_DATA + 5 PKT_CHAT binary frames
I (...) MAIN: [SIM] frames injected, entering main loop
[RIVR-TX] <bytes len=...>              ← usb_print: PKT_CHAT passes filter
I (...) RIVR_SINK: rf_tx: queued ...   ← rf_tx: 5 CHAT frames queued
I (...) MAIN: [SIM] TX type=CHAT ...   ← tx_drain_loop output (×5)
```

### Hardware mode (SX1262 connected)

```powershell
cd e:\Projects\Rivr\rivr_core
cargo build --features ffi --release
cd ..

pio run -e esp32_hw -t upload
pio device monitor --baud 115200
```

---

## Step 2b — Build via ESP-IDF (idf.py)

```powershell
# Activate ESP-IDF environment (once per shell):
. $env:IDF_PATH\export.ps1       # PowerShell
# source $IDF_PATH/export.sh     # Linux / macOS

cd e:\Projects\Rivr

# Set target (once per checkout):
idf.py set-target esp32

# Simulation build:
idf.py -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1 build

# Hardware build (no extra flags):
idf.py build

# Flash + monitor:
idf.py -p COM3 flash monitor
```

---

## Step 3 — Verify RIVR filtering

The simulation injects 5 PKT_DATA + 5 PKT_CHAT frames.
`filter.pkt_type(1)` passes only PKT_CHAT (type byte = 1).

| Frame type | Count | Expected result |
|---|---|---|
| PKT_DATA (type 6) | 5 | **Dropped** — silent |
| PKT_CHAT (type 1) | 5 | **Pass** → `[RIVR-TX]` + rf_tx queue |

If five `[RIVR-TX]` prints appear and no DATA lines are mixed in,
the entire pipeline (FFI, filter, budget, sink dispatch) is working.

---

## VS Code extension

The extension in `tools/vscode-rivr/` adds syntax highlighting and snippets
for `.rivr` files.

### Install (development)

1. Open `tools/vscode-rivr/` in VS Code.
2. Run **Extensions: Install from VSIX…** or press **F5** to launch an
   Extension Development Host.
3. Open any `.rivr` file to see syntax highlighting.

### Snippets

| Prefix | Inserts |
|---|---|
| `src-rf` | RF source with PKT_CHAT relay |
| `src-timer` | Periodic timer source |
| `prog-beacon` | Full beacon + chat relay program |
| `prog-mesh` | Full mesh program (chat + data relay) |
| `emit` | Emit block |
| `let` | Let binding with pipeline |

---

---

## SX1262 pin mapping

Default wiring — applies to all environments.  For the repeater and client
variants the pin assignments live in the variant header
(`variants/esp32devkit_e22_900_repeater/config.h` or
`variants/esp32devkit_e22_900_client/config.h`) and are guarded with `#ifndef`,
so you can override any pin by passing `-DPIN_SX1262_SCK=<gpio>` in
`platformio.ini` or via the command line.  For `esp32_hw` and `esp32_sim` the
pins are set in `firmware_core/platform_esp32.h`.

| Signal | ESP32 GPIO | Notes |
|---|---|---|
| SCK | 18 | VSPI_CLK |
| MOSI | 23 | VSPI_MOSI |
| MISO | 19 | VSPI_MISO |
| NSS (CS) | 5 | Active low |
| BUSY | 32 | SX1262 ready flag |
| RESET | 25 | Active low |
| DIO1 (IRQ) | 33 | TxDone / RxDone / Timeout |
| RXEN | 14 | Antenna switch — HIGH = receive |
| TXEN | 13 | Antenna switch — HIGH = transmit (also via SX1262 DIO2) |

For the repeater / client variants, change the value inside the variant header
or pass `-DPIN_SX1262_NSS=<gpio>` as a build flag — the `#ifndef` guard will
honour the override.

---

## Variant headers

Variant headers live under `variants/<board>/config.h` and are force-included
by PlatformIO's `-include` build flag.  Every macro they set is wrapped in
`#ifndef`, so any `-D` override wins.

Create a new variant header to port to a different board:

```c
// variants/my_custom_board/config.h
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ  915000000UL   // AU915
#endif
#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK   18
#endif
// … other pins …
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER 0
#endif
```

Then add an environment to `platformio.ini`:

```ini
[env:my_custom_board]
extends = base_hw
build_flags =
    ${base_hw.build_flags}
    -include variants/my_custom_board/config.h
```

---

## Rivr Fabric tunable macros

These macros control the congestion-aware relay policy active when
`RIVR_FABRIC_REPEATER=1`.  Override them in your variant header or as
`-D` build flags.

| Macro | Default | Description |
|---|---|---|
| `RIVR_FABRIC_DROP_THRESHOLD` | `80` | Score ≥ this → **DROP** relay frame (no re-broadcast) |
| `RIVR_FABRIC_DELAY_THRESHOLD` | `50` | Score ≥ this → **DELAY** relay by up to `MAX_EXTRA_DELAY_MS` |
| `RIVR_FABRIC_LIGHT_DELAY_THRESHOLD` | `20` | Score ≥ this → short jitter delay (low congestion) |
| `RIVR_FABRIC_MAX_EXTRA_DELAY_MS` | `1000` | Maximum added delay in milliseconds when score triggers DELAY |
| `RIVR_FABRIC_BLACKOUT_GUARD_SCORE` | `95` | At this score or above, DELAY is used instead of DROP to prevent a total relay blackout |

The score is computed over a 60-second sliding window:
```
score = clamp(rx_per_s×2 + dc_blocked_per_s×25 + tx_fail_per_s×10, 0, 100)
```

Only `PKT_CHAT` and `PKT_DATA` relay is gated by Fabric.  All other packet
types (`PKT_BEACON`, `ROUTE_REQ`, `ROUTE_RPL`, `PKT_ACK`, `PKT_PROG_PUSH`)
always pass through unaffected.

---

## Switching from simulation to hardware

1. Wire the SX1262 / E22-900M30S per the pin table above.
2. Remove `-DRIVR_SIM_MODE=1` and `-DRIVR_SIM_TX_PRINT=1` (or use `esp32_hw`).
3. Review `// TODO(SX1262):` comments in:
   - `firmware_core/main.c` — replace `radio_sim_init()` with `radio_init()`
   - `firmware_core/radio_sx1262.c` — remove `radio_init_buffers_only()` guard
4. Flash: `pio run -e esp32_hw -t upload`

### E22-900M30S module specifics

The E22-900M30S (Ebyte) requires three extra init steps compared to a bare SX1262:

| Step | SX1262 command | Why |
|---|---|---|
| `SetDio3AsTcxoCtrl(1.8 V, 5 ms)` | `0x97` | TCXO reference oscillator is powered via DIO3 |
| `Calibrate(0xFF)` | `0x89` | Full RF calibration after TCXO is stable |
| `SetPaConfig(0x04, 0x07, 0x00, 0x01)` | `0x95` | High-power PA: enables ~30 dBm output |

All three are already included in `radio_init()` in `firmware_core/radio_sx1262.c`.

### Default LoRa air parameters

| Parameter | Value |
|---|---|
| Frequency | 869.480 MHz |
| Spreading factor | SF8 |
| Bandwidth | 125 kHz (BW byte `0x04`) |
| Coding rate | 4/8 |
| Output power | +22 dBm (≈ +30 dBm after E22 PA) |
| Preamble | 8 symbols |

---

## Troubleshooting

| Symptom | Cause / Fix |
|---|---|
| `Interrupt wdt timeout on CPU0` (crash at ~30 s) | ISR tried to do SPI — ensure `radio_service_rx()` is the only SPI path; never call `sx1262_cmd` from `radio_isr()` |
| `FATAL_ERROR: librivr_core.a not found` | Run `cargo build --features ffi` first |
| `rivr_engine_init failed: code 1` | Parse error (`RIVR_ERR_PARSE`) — check `RIVR_ACTIVE_PROGRAM` or NVS program |
| `rivr_engine_init failed: code 2` | Compile error (`RIVR_ERR_COMPILE`) — check program semantics (undefined source, etc.) |
| No `[RIVR-TX]` lines | `rivr_set_emit_dispatch` not called, or `filter.pkt_type` mismatch |
| `rf_tx: queue full` warnings | Reduce frame rate or increase `RF_TX_QUEUE_CAP` in `radio_sx1262.h` |
| ESP32 crash at boot in sim mode | `platform_init()` was called — correct for hardware builds only |
| CRC failures in `protocol_decode` | Frame corrupted in ring-buffer copy; check `frame.len` bounds |
| NVS program not loading | Run `nvs_flash_erase()` once to reset partition; check `nvs_open("rivr", ...)` return value |
