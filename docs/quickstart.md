# Rivr — 5-Minute Quickstart

This guide gets you from a clean checkout to a running node in under five minutes,
first in host simulation (no hardware required) and then on real hardware.

---

## 1. Prerequisites

| Tool | Minimum version | Install |
|---|---|---|
| Rust toolchain | 1.74 | `curl https://sh.rustup.rs -sSf \| sh` |
| GCC | 11 | `sudo apt install build-essential` |
| PlatformIO CLI | 6.x | `pip install platformio` |

Clone the repository:

```bash
git clone https://github.com/MichTronics/Rivr.git Rivr
cd Rivr
```

---

## 2. Host simulation (no hardware needed)

Build the host tools and run the bundled replay:

```bash
# Build rivrc (compiler CLI) and the replay host binary
cargo build -p rivr_host

# Compile a RIVR program and display the AST
cargo run -p rivr_host --bin rivrc -- tests/rivr_replay.jsonl

# Replay a captured traffic log through the Rivr engine
cargo run -p rivr_host -- tests/rivr_replay.jsonl
```

Expected output ends with a line like:

```
[REPLAY] done — 42 frames processed, 0 errors
```

---

## 3. Run the C test suite

```bash
cd tests
make all
```

Expected: 5 suites, **295 checks total, 0 failures** (acceptance · radio\_recovery · replay · dutycycle · ota/policy).

---

## 4. Flash firmware to hardware

### ESP32 DevKit + E22-900M30S (SX1262) — primary supported board

```bash
# Flash a client node (send + receive, no relay)
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900     -t upload

# Flash the same client with BLE bridge enabled
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900_ble -t upload

# Flash a dedicated repeater node
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900   -t upload
```

> **BLE variants** — every board has a `client_<board>_ble` environment.
> The BLE client advertises as `RIVR-XXXX` and accepts GATT writes (Nordic NUS UUIDs).
> See [FLASHING.md](../FLASHING.md) for all board BLE commands.

### LilyGo LoRa32 v2.1 (SX1276 + built-in OLED)

```bash
~/.platformio/penv/bin/pio run -e client_lilygo_lora32_v21     -t upload
~/.platformio/penv/bin/pio run -e client_lilygo_lora32_v21_ble -t upload
```

### Open the serial monitor

```bash
~/.platformio/penv/bin/pio device monitor
# Baud rate: 115200
```

---

## 5. First serial session

Once the node boots you will see a startup banner followed by a `rivr>` prompt.
Common commands:

| Command | Description |
|---|---|
| `help` | List all available commands |
| `id` | Print this node's address and firmware version |
| `chat <message>` | Broadcast a CHAT packet over the mesh |
| `neighbors` | Show the neighbor link-quality table (RSSI, SNR, score) |
| `routes` | Dump the unicast reverse-path cache |
| `policy` | Print current runtime parameters (`@POLICY` JSON) |
| `supportpack` | Emit a `@SUPPORTPACK` JSON block — attach this to every bug report |

Example session:

```
rivr> id
@ID addr=0xA1B2C3D4 role=CLIENT fw=beta-0.2.0 radio=SX1262

rivr> neighbors
@NBR count=2
  peer=0xDEADBEEF rssi=-78 snr=6 score=0.82 flags=DIRECT
  peer=0xCAFEB0BA rssi=-91 snr=2 score=0.54 flags=STALE

rivr> chat Hello mesh
@CHT tx seq=1 ttl=5 len=11

rivr> supportpack
@SUPPORTPACK ...
```

---

## 6. Default radio parameters

| Parameter | Value |
|---|---|
| Frequency | 869.480 MHz (EU868 g3 sub-band) |
| Spreading factor | SF8 |
| Bandwidth | 62.5 kHz (`RF_BANDWIDTH_HZ=62500`) |
| Coding rate | CR 4/8 |
| Preamble length | 8 symbols |
| Max TX power | +30 dBm (E22) / +20 dBm (LilyGo) |
| Duty cycle | 10 % (EU868 g3 limit, enforced in firmware) |

Override frequency at build time:

```bash
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900 \
    --project-option "build_flags=-DRIVR_RF_FREQ_HZ=915000000" -t upload
```

> **Regulatory notice:** You are responsible for compliance with the radio regulations
> in your jurisdiction.  The default 869.480 MHz / +30 dBm configuration targets
> EU868 g3; operating outside the licensed band or above the permitted power level
> is illegal in most jurisdictions.

---

## 7. Next steps

| Goal | Where to look |
|---|---|
| Enable BLE companion link | [docs/en/companion-ble-integration.md](en/companion-ble-integration.md) |
| Architecture | [docs/en/architecture.md](en/architecture.md) |
| Write a custom RIVR pipeline | [docs/en/language-reference.md](en/language-reference.md) |
| Wire up new hardware | [docs/en/build-guide.md](en/build-guide.md) |
| Integrate into existing firmware | [docs/en/firmware-integration.md](en/firmware-integration.md) |
| Prepare a release | [docs/releasing.md](releasing.md) |
| Beta release checklist | [docs/beta-release-checklist.md](beta-release-checklist.md) |
