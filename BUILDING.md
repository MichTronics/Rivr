# RIVR Embedded Node — Build Guide

## Directory Structure

```
~/Rivr/
├── CMakeLists.txt          ← ESP-IDF top-level project
├── main/
│   └── CMakeLists.txt      ← ESP-IDF "main" component (sources + lib link)
├── sdkconfig.defaults      ← ESP-IDF build config overrides
├── platformio.ini          ← PlatformIO (alternative to idf.py)
├── firmware_core/          ← C hardware drivers
│   ├── main.c              ← app_main, simulation mode
│   ├── radio_sx1262.c/.h   ← SX1262 radio driver + ring-buffers
│   ├── timebase.c/.h       ← monotonic + Lamport clocks
│   ├── dutycycle.c/.h      ← LoRa duty-cycle tracker
│   ├── platform_esp32.c/.h ← GPIO/SPI/LED init
│   └── ringbuf.h           ← lock-free SPSC ring buffer
├── rivr_layer/             ← RIVR glue (C)
│   ├── rivr_embed.c/.h     ← engine init, rivr_tick()
│   ├── rivr_sinks.c/.h     ← rf_tx + usb_print + log sinks
│   ├── rivr_sources.c/.h   ← rf_rx + cli + timer sources
│   └── rivr_programs/
│       └── default_program.h
└── rivr_core/              ← Rust library (parser + compiler + runtime)
    └── src/ffi.rs          ← #[no_mangle] C-ABI exports
```

---

## Step 0: Prerequisites

| Tool          | Minimum version | Install                                          |
|---------------|-----------------|--------------------------------------------------|
| Rust          | 1.75+           | `rustup update stable`                           |
| ESP-IDF       | 5.1             | https://docs.espressif.com/projects/esp-idf/en/  |
| PlatformIO    | 6.x (optional)  | `pip install platformio`                         |
| espup         | optional        | For Xtensa cross-compilation target              |

---

## Step 1: Build the Rust Static Library

The Rust library must be compiled **before** the C firmware.

### Debug build (recommended for development)

```bash
cd ~/Rivr/rivr_core
cargo build --features ffi
# Library at:  target/debug/librivr_core.a
```

### Release build (for flashing to device)

```bash
cargo build --features ffi --release
# Library at:  target/release/librivr_core.a
```

### Cross-compile for ESP32 target (requires espup toolchain)

```bash
# Install Xtensa Rust toolchain once:
espup install

# Then cross-compile:
cargo +esp build --target xtensa-esp32-espidf --features ffi --release
# Library at:  target/xtensa-esp32-espidf/release/librivr_core.a
```

> **Note:** `main/CMakeLists.txt` automatically finds `librivr_core.a` by searching
> `rivr_core/target/release/` then `rivr_core/target/debug/`. No manual copy needed.

---

## Step 2a: Build + Flash via PlatformIO (recommended)

### Simulation mode (no SX1262 hardware)

```bash
cd ~/Rivr

# Build simulation firmware
pio run -e esp32_sim

# Flash to ESP32 + open serial monitor
pio run -e esp32_sim -t upload
pio device monitor -e esp32_sim --baud 115200
```

**Expected UART output:**
```
I (...) MAIN: ═══ RIVR Embedded Node booting ═══
I (...) MAIN: *** SIMULATION MODE: no real SX1262 hardware ***
I (...) RADIO: radio_init_buffers_only: ringbufs ready (SIM MODE)
I (...) RIVR_SINK: sinks registered: rf_tx, usb_print, log
I (...) RIVR_EMBED: rivr_emit_dispatch wired to Rust engine
I (...) RIVR_EMBED: rivr_embed_init: engine ready
I (...) MAIN: [SIM] injecting 5 DATA + 5 CHAT frames into rf_rx_ringbuf
I (...) MAIN: [SIM] frames injected, entering main loop (rivr_tick will process them)
I (...) MAIN: entering main loop
--- RIVR processes frames across 3 ticks (SOURCES_RF_RX_DRAIN_LIMIT=4/tick) ---
I (...) RIVR_SRC: rf_rx: ... (injected events logged at DEBUG level)
[RIVR-TX] CHAT:hello-0          ← usb_print sink: CHAT frames pass filter
[RIVR-TX] CHAT:hello-1
[RIVR-TX] CHAT:hello-2
[RIVR-TX] CHAT:hello-3
[RIVR-TX] CHAT:hello-4
I (...) RIVR_SINK: rf_tx: queued N bytes (toa_approx=... us, type=0x04)   × 5
I (...) MAIN: [SIM] TX type=CHAT lmp=5 len=10 payload="hello-0" toa=... us
... (5 TX drain lines)
```

### Hardware mode (real SX1262 connected)

```bash
cd ~/Rivr

# First build the release Rust library:
cd rivr_core && cargo build --features ffi --release && cd ..

# Build + flash hardware firmware
pio run -e esp32_hw -t upload
pio device monitor --baud 115200
```

---

## Step 2b: Build via ESP-IDF (idf.py)

```bash
# Activate ESP-IDF environment (run once per shell):
. $IDF_PATH/export.sh

cd ~/Rivr

# Select target (once):
idf.py set-target esp32

# Build (debug, default):
idf.py build

# For simulation mode, add the compiler flag:
idf.py -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1 build

# Flash + monitor (replace /dev/ttyUSB0 with your device):
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## Step 3: Verify RIVR filtering

The proof-of-life output verifies the full pipeline:

| Frame type | Count | Expected result                                    |
|------------|-------|----------------------------------------------------|
| DATA       |   5   | **Filtered out** by `filter.kind("CHAT")` — silent |
| CHAT       |   5   | **Pass through** → `[RIVR-TX]` print + rf_tx queue |

If the `[RIVR-TX]` lines appear and DATA lines do NOT appear mixed in,
the RIVR engine, FFI bridge, filter, and sink dispatch are all working.

---

## Connecting Real SX1262 Hardware

1. Wire SX1262 module to ESP32 per the pin map in `firmware_core/platform_esp32.h`
   (defaults: SCK=18, MOSI=27, MISO=19, NSS=5, BUSY=26, RESET=14, DIO1=33).
2. Remove `-DRIVR_SIM_MODE=1` and `-DRIVR_SIM_TX_PRINT=1` from the build flags
   (or use the `esp32_hw` PlatformIO environment which already excludes them).
3. Flash with `pio run -e esp32_hw -t upload`.

---

## Host Unit Tests (CI / no hardware required)

All C-layer test suites compile and run on Linux/macOS with plain `gcc` — no
ESP-IDF toolchain, no Rust cross-compiler, and no hardware.

### Run all host suites

```bash
make -C tests          # build + run all: acceptance, recovery, replay, dutycycle, policy, ota
make -C tests ota      # OTA gate tests only  (10 tests, signed build)
make -C tests asan     # rebuild with AddressSanitizer + UBSan across all suites
```

### OTA isolation guarantee

`firmware_core/rivr_ota_core.c` is ESP-IDF-free and is the only OTA file
linked in host tests.  `firmware_core/rivr_ota_platform.c` (NVS backend) is
never referenced by the `tests/` Makefile.  Tests provide the
`ota_platform_*` and `ota_storage_*` stubs inline in `tests/test_ota.c`.

### CI pipeline hint (GitHub Actions / GitLab CI)

```yaml
- name: Host unit tests (no hardware)
  run: |
    make -C tests acceptance
    make -C tests ota
    make -C tests dutycycle
    make -C tests replay
    make -C tests policy
```

---

## Troubleshooting

| Symptom                                   | Cause / Fix                                              |
|-------------------------------------------|----------------------------------------------------------|
| `FATAL_ERROR: librivr_core.a not found`   | Run `cargo build --features ffi` in `rivr_core/` first  |
| `rivr_engine_init failed: -1`             | RIVR program parse error — check `RIVR_ACTIVE_PROGRAM`  |
| `rivr_engine_init failed: -2`             | RIVR compiler error — check program semantics           |
| No `[RIVR-TX]` lines appear               | `rivr_set_emit_dispatch` not called, or filter mismatch |
| `rf_tx: queue full` warnings              | Lower `TX_DRAIN_LIMIT` or increase `RF_TX_QUEUE_CAP`    |
| ESP32 crash in `radio_sim_init`           | `platform_init()` was called — that's correct for hw    |
