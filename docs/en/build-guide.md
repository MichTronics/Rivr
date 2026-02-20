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
cd e:\Projects\Rivr\rivr_core
cargo run -p rivr_host
```

Runs 8 demos covering every operator category plus Replay 2.0.

---

## Step 2a — Build via PlatformIO *(recommended)*

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

## SX1262 pin mapping

Default wiring from `firmware_core/platform_esp32.h`:

| Signal | ESP32 GPIO |
|---|---|
| SCK | 18 |
| MOSI | 27 |
| MISO | 19 |
| NSS (CS) | 5 |
| BUSY | 26 |
| RESET | 14 |
| DIO1 (IRQ) | 33 |

Adjust pin defines in `platform_esp32.h` to match your board.

---

## Switching from simulation to hardware

1. Wire the SX1262 per the pin table above.
2. Remove `-DRIVR_SIM_MODE=1` and `-DRIVR_SIM_TX_PRINT=1` (or use `esp32_hw`).
3. Review `// TODO(SX1262):` comments in:
   - `firmware_core/main.c` — replace `radio_sim_init()` with `radio_init()`
   - `firmware_core/radio_sx1262.c` — remove `radio_init_buffers_only()` guard
4. Flash: `pio run -e esp32_hw -t upload`

---

## Troubleshooting

| Symptom | Cause / Fix |
|---|---|
| `FATAL_ERROR: librivr_core.a not found` | Run `cargo build --features ffi` first |
| `rivr_engine_init failed: -1` | Parse error in RIVR program — check `RIVR_ACTIVE_PROGRAM` |
| `rivr_engine_init failed: -2` | Compiler error — check program semantics (undefined source, etc.) |
| No `[RIVR-TX]` lines | `rivr_set_emit_dispatch` not called, or `filter.pkt_type` mismatch |
| `rf_tx: queue full` warnings | Reduce frame rate or increase `RF_TX_QUEUE_CAP` in `radio_sx1262.h` |
| ESP32 crash at boot in sim mode | `platform_init()` was called — correct for hardware builds only |
| CRC failures in `protocol_decode` | Frame corrupted in ring-buffer copy; check `frame.len` bounds |
