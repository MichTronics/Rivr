# Rivr — Flashing Guide

This guide covers every supported board from prerequisites to a running node.
Start here if you have hardware in hand and want the fastest path to a working mesh.

---

## Prerequisites

| Tool | Minimum version | Install |
|---|---|---|
| Python 3 | 3.8 | `sudo apt install python3 python3-pip` |
| PlatformIO CLI | 6.x | `pip install platformio` |
| Rust toolchain | 1.74 | `curl https://sh.rustup.rs -sSf \| sh` |
| esptool | bundled with PlatformIO | — |
| adafruit-nrfutil | 0.5.3+ | `pip install adafruit-nrfutil` *(nRF52840 boards only)* |
| picotool | 1.x | `sudo apt install picotool` *(RP2040 boards only)* |

Plug the board in via USB.  Confirm the device appears:

```bash
ls /dev/ttyUSB*   # CP210x / CH340
ls /dev/ttyACM*   # native USB (ESP32-S3 boards)
```

Grant yourself access (one-off):

```bash
sudo usermod -aG dialout $USER
# Log out and back in, or:
newgrp dialout
```

---

## Step 1 — Build the Rust library

The Rust static library must be compiled before the C firmware can link.
Choose the command for your board's MCU family.

```bash
cd ~/Rivr

# ── ESP32 (Xtensa LX6) — DevKit, LilyGo v2.1, Heltec V2, T-Beam ──────────
cargo +esp build -p rivr_core --target xtensa-esp32-espidf --features ffi --release

# ── ESP32-S3 (Xtensa LX7) — Heltec V3/V4, LilyGo T3-S3, Seeed XIAO ────────
cargo +esp build -p rivr_core --target xtensa-esp32s3-espidf --features ffi --release

# ── nRF52840 (Cortex-M4F) — RAK4631, Heltec T114, Seeed T1000-E ────────────
cargo build -p rivr_core --target thumbv7em-none-eabihf \
    --no-default-features --features ffi --release \
    -Zbuild-std=core,alloc,panic_abort

# ── RP2040 (Cortex-M0+) — Waveshare RP2040-LoRa-HF ─────────────────────────
cargo build -p rivr_core --target thumbv6m-none-eabi \
    --no-default-features --features ffi --release \
    -Zbuild-std=core,alloc,panic_abort
```

> **ESP32 / ESP32-S3 first time?**  Install the Xtensa toolchain once with:
> `cargo install espup && espup install`
>
> **nRF52840 / RP2040 first time?**  Install the Rust targets once with:
> ```bash
> rustup target add thumbv7em-none-eabihf
> rustup target add thumbv6m-none-eabi
> rustup component add rust-src   # needed for -Zbuild-std
> ```
> `-Zbuild-std` requires the nightly toolchain (`rustup default nightly`) or a
> per-project `rust-toolchain.toml` calling `channel = "nightly"`.

---

## Step 2 — Flash firmware

Pick the command for your board. All environments flash at 921600 baud.

> **BLE client variants** — every board has a `client_<board>_ble` environment that enables
> the BLE bridge in addition to the standard LoRa client firmware. The node advertises
> as `RIVR-XXXX` and accepts GATT writes using Nordic NUS UUIDs. Replace any `client_*`
> environment below with its `_ble` counterpart to flash the BLE-enabled build.

---

### ESP32 DevKit V1 + EBYTE E22-900M30S — **primary supported board**

```bash
# Client node (send/receive, serial CLI, no relay)
pio run -e client_esp32devkit_e22_900     -t upload

# Client + BLE bridge (advertises as "RIVR-XXXX" over BLE)
pio run -e client_esp32devkit_e22_900_ble -t upload

# Dedicated repeater (relay with congestion scoring, no CLI chat)
pio run -e repeater_esp32devkit_e22_900   -t upload
```

Pin wiring for this board is in `variants/esp32devkit_e22_900/config.h`.

---

### LilyGo LoRa32 v2.1 — SX1276, built-in OLED

```bash
pio run -e client_lilygo_lora32_v21       -t upload
pio run -e client_lilygo_lora32_v21_ble   -t upload
pio run -e repeater_lilygo_lora32_v21     -t upload
```

---

### Heltec WiFi LoRa 32 V3 — ESP32-S3, SX1262, OLED

```bash
pio run -e client_heltec_lora32_v3        -t upload
pio run -e client_heltec_lora32_v3_ble    -t upload
pio run -e repeater_heltec_lora32_v3      -t upload
```

> Use the `xtensa-esp32s3-espidf` Rust target for this board.

---

### Heltec WiFi LoRa 32 V2 — ESP32, SX1276, OLED

```bash
pio run -e client_heltec_lora32_v2        -t upload
pio run -e client_heltec_lora32_v2_ble    -t upload
pio run -e repeater_heltec_lora32_v2      -t upload
```

---

### LilyGo T-Beam v1.1 (SX1262) — ⚠ Experimental

```bash
pio run -e client_lilygo_tbeam_sx1262     -t upload
pio run -e client_lilygo_tbeam_sx1262_ble -t upload
pio run -e repeater_lilygo_tbeam_sx1262   -t upload
```

> ⚠ The AXP192 PMIC must be initialised before the radio powers on.
> See `platform_esp32.c` (`RIVR_TBEAM_AXP192=1`) for the required stub.

---

### LilyGo T3-S3 — ESP32-S3, SX1262, OLED

```bash
pio run -e client_lilygo_t3s3             -t upload
pio run -e client_lilygo_t3s3_ble         -t upload
pio run -e repeater_lilygo_t3s3           -t upload
```

---

### Heltec WiFi LoRa 32 V4 — ESP32-S3, SX1262, OLED

Build the Rust library for ESP32-S3 first (see Step 1).

```bash
pio run -e client_heltec_lora32_v4        -t upload
pio run -e client_heltec_lora32_v4_ble    -t upload
pio run -e repeater_heltec_lora32_v4      -t upload
```

Connects via native USB (CDC); device appears as `/dev/ttyACM*`.

---

### Seeed XIAO ESP32S3 + Wio-SX1262 — ESP32-S3, SX1262

Build the Rust library for ESP32-S3 first (see Step 1).

```bash
pio run -e client_seeed_xiao_sx1262       -t upload
pio run -e client_seeed_xiao_sx1262_ble   -t upload
pio run -e repeater_seeed_xiao_sx1262     -t upload
```

Connects via native USB (CDC); device appears as `/dev/ttyACM*`.

---

### RAK WisMesh / WisPocket (RAK4631) — nRF52840, SX1262

Build the Rust library for nRF52840 first (see Step 1 — `thumbv7em-none-eabihf`).

The board uses the Adafruit nRF52 USB bootloader for DFU upload. Plug in via USB.

```bash
pio run -e client_rak4631                 -t upload
pio run -e client_rak4631_ble             -t upload
pio run -e repeater_rak4631               -t upload
```

> **Bootloader not entered?** Double-tap the RESET button quickly — the LED
> turns on steadily when the bootloader is active. Device appears as
> `/dev/ttyACM*`. PlatformIO calls `adafruit-nrfutil` automatically.

---

### Heltec T114 — nRF52840, SX1262, OLED

Build the Rust library for nRF52840 first (see Step 1 — `thumbv7em-none-eabihf`).

```bash
pio run -e client_heltec_t114             -t upload
pio run -e client_heltec_t114_ble         -t upload
pio run -e repeater_heltec_t114           -t upload
```

> Double-tap RESET to enter the nRF52 bootloader if PlatformIO cannot connect.

---

### Seeed SenseCAP T1000-E — nRF52840, LR1110 ⚠ Experimental

Build the Rust library for nRF52840 first (see Step 1 — `thumbv7em-none-eabihf`).

```bash
pio run -e client_seeed_t1000_e           -t upload
pio run -e client_seeed_t1000_e_ble       -t upload
pio run -e repeater_seeed_t1000_e         -t upload
```

> This board uses the LR1110 radio via RadioLib. Double-tap RESET to enter the
> nRF52 bootloader. Tracker GPS functionality is not yet integrated into Rivr.

---

### Waveshare RP2040-LoRa-HF — RP2040, SX1262

Build the Rust library for RP2040 first (see Step 1 — `thumbv6m-none-eabi`).

```bash
pio run -e client_waveshare_rp2040_lora_hf   -t upload
pio run -e repeater_waveshare_rp2040_lora_hf -t upload
```

PlatformIO uses `picotool` for upload. The board must be in BOOTSEL mode:
hold the **BOOT** button while plugging in USB — it appears as a USB mass
storage device. PlatformIO enters BOOTSEL mode automatically on most systems.

**Alternative — drag-and-drop UF2:**
```bash
# Build the UF2 file without flashing:
pio run -e client_waveshare_rp2040_lora_hf
# Then copy the .uf2 from .pio/build/client_waveshare_rp2040_lora_hf/ to
# the RPI-RP2 mass storage drive.
```

---

## Step 3 — Open the serial monitor

```bash
pio device monitor --baud 115200
```

A successful boot prints:

```
╔══════════════════════════════════╗
║   Rivr Client Node — Serial CLI  ║
╚══════════════════════════════════╝
Node ID  : 0xA3F27C01
Callsign : NODE1
Net ID   : 0x0000
Type 'help' for full command list.
>
```

---

## Step 4 — First CLI session

| Command | What it does |
|---|---|
| `help` | List all commands |
| `id` | Confirm node address and firmware version |
| `status` | Routing table, frame counters, duty-cycle |
| `neighbors` / `peers` | Live link-quality table |
| `chat hello` / `send hello` | Broadcast a CHAT frame to the mesh |
| `stats` / `metrics` | Emit a full @MET JSON snapshot |
| `supportpack` | Capture diagnostics for a bug report |
| `reboot` | Software reset the device |

---

## Changing radio parameters

The primary E22-900 preset defaults to EU868 g3 (869.480 MHz, SF8, BW 62.5 kHz via `RF_BANDWIDTH_HZ=62500`, CR 4/8).
To change them, copy `user_config_template.h` to your variant's `config.h` and
uncomment the relevant lines, or add `-D` flags in `platformio.ini`:

```ini
[env:client_esp32devkit_e22_900]
build_flags =
    ${env.build_flags}
    -DRIVR_RF_FREQ_HZ=915000000   ; AU915 / US915
    -DRF_TX_POWER_DBM=14
    -include ${PROJECT_DIR}/variants/esp32devkit_e22_900/config.h
```

All nodes in the same mesh **must** use the same frequency, SF, BW, and CR.

---

## Troubleshooting

| Symptom | Cause / Fix |
|---|---|
| `FATAL_ERROR: librivr_core.a not found` | Run the `cargo +esp build` step in Step 1 first |
| `dfu-util: No DFU capable USB device found` | Board needs to be in bootloader mode — hold BOOT while pressing RESET |
| `A fatal error occurred: Failed to connect` | Wrong port, bad cable, or missing driver — check `ls /dev/ttyUSB*` |
| No output in serial monitor | Wrong baud rate (must be 115200) or wrong port |
| Radio silent (no neighbours ever appear) | Frequency/SF/BW mismatch between nodes; check `id` and `status` |
| Node does not relay | This is a client node — use a `repeater_*` environment for relay |
| BLE not advertising | Build did not use a `client_*_ble` environment; check `RIVR_FEATURE_BLE` and `sdkconfig.ble` |
| AXP192 / T-Beam hangs at boot | PMIC init stub required — see T-Beam note above |

> **Bug reports:** always attach the output of `supportpack` from the serial monitor.
