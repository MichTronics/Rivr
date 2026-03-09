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

```bash
cd rivr_core
cargo build --features ffi
# Output:  target/debug/librivr_core.a
```

### Release (for flashing)

```bash
cargo build --features ffi --release
# Output:  target/release/librivr_core.a
```

### Cross-compile for ESP32 (Xtensa LX6)

```bash
# Install the Xtensa Rust toolchain (once):
espup install

# Cross-compile:
cargo +esp build --target xtensa-esp32-espidf --features ffi --release
# Output:  target/xtensa-esp32-espidf/release/librivr_core.a
```

> `main/CMakeLists.txt` automatically finds the library — it searches
> `target/release/` first, then `target/debug/`. No manual copy needed.

### Host demos (no hardware)

```bash
cargo run -p rivr_host
```

Runs 8 demos covering every operator category plus Replay 2.0.

### `rivrc` — RIVR source compiler CLI

```bash
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

### Client node serial CLI

The client firmware (`client_esp32devkit_e22_900`) activates an interactive
chat shell over UART0 at 115200 baud.  After flashing, open the serial monitor:

```bash
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900 -t upload
~/.platformio/penv/bin/pio device monitor -e client_esp32devkit_e22_900
```

**Boot banner:**
```
╔══════════════════════════════════╗
║   Rivr Client Node — Serial CLI  ║
╚══════════════════════════════════╝
Node ID  : 0xDEADBEEF
Callsign : CALL
Net ID   : 0x0000
Type 'help' for full command list.
> 
```

**Commands:**

| Command | Effect |
|---|---|
| `chat <message>` | Encode and broadcast a `PKT_CHAT` frame over LoRa |
| `id` | Print this node’s 32-bit ID, callsign and net ID |
| `info` | Print build info (env, git SHA, radio profile) |
| `metrics` | Print all counters/gauges as JSON (`@MET` line) |
| `policy` | Print `@POLICY` JSON (current params + cumulative metrics) |
| `supportpack` | JSON dump: build info + full metrics snapshot (`@SUPPORTPACK`) |
| `neighbors` | Show live neighbour table with RSSI/SNR/link scores |
| `routes` | Show route cache with scores and ages |
| `set callsign <CS>` | Set and persist callsign (1–11 chars: A–Z a–z 0–9 -) |
| `set netid <HEX>` | Set and persist network ID (hex 0…FFFF) |
| `log <debug\|metrics\|silent>` | Set log verbosity |
| `help` | Show this command list |

**Incoming messages** from other mesh nodes are printed automatically:

```
[CHAT][cafebabe]: hello from the repeater
> 
```

The `> ` prompt is reprinted after every incoming message so in-progress
input is not lost.  Maximum message length is 128 characters (line buffer);
messages are encoded using the standard `PKT_CHAT` binary wire format.

### Simulation mode (no SX1262 hardware required)

```bash
# Build simulation firmware
pio run -e esp32_sim

# Flash + open serial monitor
pio run -e esp32_sim -t upload
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

```bash
cd rivr_core && cargo build --features ffi --release && cd ..
pio run -e esp32_hw -t upload
pio device monitor --baud 115200
```

---

## Step 2b — Build via ESP-IDF (idf.py)

```bash
# Activate ESP-IDF environment (once per shell):
source $IDF_PATH/export.sh

# Set target (once per checkout):
idf.py set-target esp32

# Simulation build:
idf.py -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1 build

# Hardware build (no extra flags):
idf.py build

# Flash + monitor (replace /dev/ttyUSB0 with your port):
idf.py -p /dev/ttyUSB0 flash monitor
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

### Signed `@PARAMS` flags

For production deployments that want to authenticate over-the-air parameter
updates, add these flags to your variant header or `platformio.ini`:

```ini
build_flags =
    ; require HMAC-SHA-256 MAC on every @PARAMS frame
    -D RIVR_FEATURE_SIGNED_PARAMS=1
    -D RIVR_FEATURE_ALLOW_UNSIGNED_PARAMS=0
    ; 32-byte PSK as 64 hex characters (CHANGE THIS)
    -D RIVR_PARAMS_PSK_HEX='\"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\"'
```

Generate a signed `@PARAMS` string:

```bash
MSG='@PARAMS beacon=30000 txpow=14'
SIG=$(printf '%s' "$MSG" | openssl dgst -sha256 \
      -mac hmac -macopt hexkey:"$PSK_HEX" -binary | xxd -p -c 256)
echo "$MSG sig=$SIG"
```

---

## OLED display wiring (SSD1306 128×64)

All hardware environments enable the SSD1306 display driver (`FEATURE_DISPLAY=1`).
The driver runs as a low-priority FreeRTOS task on CPU1 and handles all I²C
traffic without blocking the main task.

### Default I²C pins

| Signal | ESP32 GPIO | Notes |
|---|---|---|
| SDA | 21 | Override with `-DPIN_DISPLAY_SDA=<gpio>` |
| SCL | 22 | Override with `-DPIN_DISPLAY_SCL=<gpio>` |
| VCC | 3.3 V | Do **not** connect to 5 V; SSD1306 I²C lines are 3.3 V |
| GND | GND | |

Pull-ups (4.7 kΩ to 3.3 V) are required on SDA and SCL; most SSD1306 breakout
boards include them.  The ESP-IDF I²C master driver also enables weak internal
pull-ups automatically.

### I²C parameters

| Parameter | Value |
|---|---|
| Speed | 400 kHz (fast mode) |
| Addressing mode | Horizontal (Adafruit-compatible); entire 1024-byte GDDRAM flushed in a single 1025-byte burst |
| Address detection | Auto; tries 0x3C (“SA0 low”) then 0x3D (“SA0 high”) at boot |
| Refresh rate | Max 5 Hz (200 ms guard); page auto-rotates every 3 s |

### Feature gate

```c
// Enable in platformio.ini build_flags:
-DFEATURE_DISPLAY=1

// Or in a variant header:
#define FEATURE_DISPLAY 1
```

Without `FEATURE_DISPLAY=1` all display functions compile to zero-cost inline
no-ops; no I²C bus is initialised.

### Boot sequence

1. I²C bus created; 150 ms VCC settling delay.
2. Address auto-detected (0x3C probed first).
3. Full init sequence sent (display off → clock div → mux → charge pump →
   horizontal mode → segment remap → COM scan → contrast → display on).
4. **Self-test** (400 ms): `0xA5` forces all pixels ON — display shows solid white.
   If the screen stays black here the hardware path between ESP32 and OLED has a fault.
5. Boot screen displayed (node ID, net ID, callsign).
6. Normal 5 Hz refresh begins.

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
| OLED shows nothing after boot | Check SDA/SCL wiring (default GPIO21/22), 3.3 V VCC, and 4.7 kΩ pull-ups on SDA+SCL |
| `SSD1306 not found on SDA=GPIO21 SCL=GPIO22` | Chip not ACKing at 0x3C or 0x3D; check SA0 pin on module and power supply |
| OLED shows white at boot then shows boot screen | **Normal —** 400 ms all-pixels-ON self-test, then boot screen displays |
| OLED freezes / goes blank after TX | Supply glitch from high-power PA; driver auto-reinits after 3 flush failures; add bulk decoupling near OLED VCC |
