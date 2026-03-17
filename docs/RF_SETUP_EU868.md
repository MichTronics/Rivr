# RF Setup — EU868

Configuration guide for operating RIVR nodes on the EU868 band using EBYTE E22-900M30S
or E22-900M33S modules (SX1262, high-power PA).

---

## 1. Legal Framework (EU868)

| Parameter | Value | Source |
|---|---|---|
| Band | 863–870 MHz | ETSI EN 300 220 |
| Sub-band used | 869.4–869.650 MHz | g3 — 27 dBm ERP, 10% duty |
| Default channel | **869.480 MHz** | firmware default |
| Max conducted power | 30 dBm (1 W) | E22-900M30S rated |
| Effective radiated power | ≤ 27 dBm ERP | depends on antenna gain |
| Duty cycle | **≤ 1 %** per hour | RIVR default (`dutycycle.c`) |
| Duty cycle — sub-band g3 | ≤ 10 % | legal maximum for 869.4–869.65 |

> **RIVR enforces 1 % (36 s/hour)** by default — well within the legal 10 % limit.
> Raise the budget only if you have verified your deployment channel is g3 **and** you
> have legal authority to use the higher limit.

---

## 2. Default RF Parameters

```c
// firmware_core/radio_sx1262.c defaults
Frequency   : 869.480 MHz
Spreading Factor : SF8
Bandwidth   : 62.5 kHz (`RF_BANDWIDTH_HZ=62500`)
Coding Rate : CR 4/8 (4/8 = 0.5 code rate)
Preamble    : 8 symbols
TX power    : 22 dBm (SX1262 SetTxParams)
```

### Air-time for typical RIVR frames

| Frame type | Bytes | SF8/BW62.5/CR4-8 ToA |
|---|---|---|
| BEACON (min) | 35 | ~140 ms |
| CHAT (short, 20-byte payload) | 46 | ~184 ms |
| CHAT (max, 231-byte payload) | 257 | ~720 ms |
| ROUTE_REQ / ROUTE_RPL | 24–35 | ~110–140 ms |

> ToA values are approximate at SF8/BW62.5. Use the Semtech LoRa Calculator or the
> `RIVR_TOA_APPROX_US` macro for budget planning.

---

## 3. Channel Plan

### EU868 common channels

| Channel | Frequency | Notes |
|---|---|---|
| g1-0 | 868.100 MHz | Default LoRaWAN uplink ch 0 |
| g1-1 | 868.300 MHz | Default LoRaWAN uplink ch 1 |
| g1-2 | 868.500 MHz | Default LoRaWAN uplink ch 2 |
| **g3** | **869.480 MHz** | **RIVR default — 10 % duty, 27 dBm ERP** |
| g3-alt | 869.525 MHz | Alternative g3 channel |

Override at build time:

```bash
# Build for EU868 channel 0
pio run -e repeater_esp32devkit_e22_900 \
  --build-option='build_flags=-DRIVR_RF_FREQ_HZ=868100000'

# Build for AU915 (check local regulations)
pio run -e repeater_esp32devkit_e22_900 \
  --build-option='build_flags=-DRIVR_RF_FREQ_HZ=915000000'

# Build for AS923
pio run -e repeater_esp32devkit_e22_900 \
  --build-option='build_flags=-DRIVR_RF_FREQ_HZ=923000000'
```

Or add to `platformio.ini` **before** the `-include` line (the `#ifndef` guard in the
variant header will not override a `-D` flag):

```ini
build_flags =
    -DRIVR_RF_FREQ_HZ=868100000
    -include variants/esp32devkit_e22_900_repeater/config.h
```

---

## 4. Antenna

### Antenna selection

| Antenna type | Gain | Notes |
|---|---|---|
| Rubber duck (λ/4) | 0 dBi | Minimum; indoor range ~200 m |
| Fibreglass ground-plane | 3–5 dBi | Outdoor; range 1–3 km LOS |
| Yagi / directional | 7–12 dBi | Point-to-point link; requires EIRP calculation |

### EIRP budget

```
EIRP (dBm) = TX power − cable loss + antenna gain
           ≤ 27 dBm ERP (≈ 29.15 dBm EIRP) for sub-band g3

Example:
  TX power     = 22 dBm  (E22-900M30S default, SX1262 register)
  Cable loss   =  1 dB   (1 m low-loss coax)
  Antenna gain = +3 dBi  (fibreglass omni)
  ──────────────────────
  EIRP         = 24 dBm  ✅ within limit
```

### Physical installation

- Mount antenna **vertically** (linear vertical polarisation matches most devices).
- Keep antenna ≥ 20 cm from metal enclosures.
- Use a ground plane (≥ λ/4 = ~8 cm radius) for λ/4 whip antennas.
- Avoid routing coax near switching power supplies; use N-type or SMA connectors.
- For outdoor installations: weatherproof connector with self-amalgamating tape.

---

## 5. GPIO / SPI Wiring (E22-900M30S)

| SX1262 signal | E22-900M30S pin | ESP32 DevKit GPIO | Direction |
|---|---|---|---|
| SCK | 7 | 18 | MCU → radio |
| MOSI | 6 | 23 | MCU → radio |
| MISO | 5 | 19 | MCU ← radio |
| NSS/CS | 8 | 5 | MCU → radio |
| BUSY | 9 | 32 | MCU ← radio |
| RESET | 10 | 25 | MCU → radio |
| DIO1 | 11 | 33 | MCU ← radio (IRQ) |
| RXEN | M1 | 14 | MCU → radio (RF switch) |
| TXEN | M0 | 13 | MCU → radio (RF switch) |
| GND | GND | GND | — |
| 3.3 V | VCC | 3.3 V | — |

> The E22-900M30S/M33S has an internal RF switch controlled by RXEN/TXEN.
> Both pins must be driven correctly or sensitivity and TX power will be degraded.
> `RIVR_RFSWITCH_ENABLE=1` handles this automatically when you use the provided variant headers.

Override any pin at build time:

```bash
pio run -e repeater_esp32devkit_e22_900 \
  --build-option='build_flags=-DPIN_SX1262_BUSY=34 -DPIN_SX1262_DIO1=35'
```

---

## 6. Duty-Cycle Budget

RIVR tracks duty cycle in `dutycycle.c` using a 1-hour sliding window.

### How it works

```
For each transmitted frame:
  tx_end_ms = tb_millis() + toa_ms
  Budget used += toa_ms

Sliding window purges events older than 3600 s.
Remaining budget = 36 000 ms − sum(active_frame_toa)

If remaining budget < toa of next frame:
  → duty_blocked++
  → frame is dropped (not queued)
```

### Budget planning guide

At SF8/BW62.5/CR4-8 each CHAT frame ≈ 184 ms ToA.

| Scenario | Frames/hour | ToA/hour | Duty | Status |
|---|---|---|---|---|
| 1 node chatting freely | 100 | 9 200 ms | 0.26 % | ✅ |
| 5-node mesh, all relaying | 500 | 46 000 ms | 1.28 % | ⚠️ relay limit activates |
| 5-node + sensor beacon every 30 s | 620 | ~57 000 ms | 1.58 % | ❌ `duty_blocked` fires |

### Tuning

1. Increase beacon interval (`rivr_embed.c`, `timer(30000)` → `timer(60000)`).
2. Reduce TX power to shorten ToA qualification threshold (no change to time-on-air).
3. Use SF7 instead of SF8 — effectively halves ToA at the cost of ~3 dB sensitivity.
4. Split the network onto two frequency channels with separate `RIVR_RF_FREQ_HZ`.

---

## 7. Modulation Variants

To change spreading factor or bandwidth, edit `radio_sx1262.c` `radio_init()`:

```c
// SF7 / BW 62.5 kHz — faster than SF8, uses SX1262 BW byte 0x03
radio_set_modulation_params(h, 7, 0x03, LORA_CR_4_8, 0);

// SF10 / BW 62.5 kHz — slower, longer range, ~4× longer ToA
radio_set_modulation_params(h, 10, 0x03, LORA_CR_4_8, 0);

// SF8 / BW 250 kHz — same range as SF8/125 but ~2× faster
radio_set_modulation_params(h, 8, 0x05, LORA_CR_4_8, 0);
```

> **Important:** All nodes in a network must use identical modulation parameters; LoRa
> frames from nodes with different SF/BW are invisible to each other.

---

## 8. RF Troubleshooting

| Symptom | Likely cause | First action |
|---|---|---|
| `rad_crc` counter rising | Signal too weak or interference | Check RSSI on sender side; increase TX power or reduce distance |
| `rad_stall` rising | BUSY pin stuck high | Check RESET/BUSY wiring; verify 3.3 V supply current ≥ 200 mA |
| `dc_blk` non-zero | Duty cycle budget exhausted | See Section 6; increase beacon interval or use SF lower number |
| No RX after reset | RXEN not driven | Verify `RIVR_RFSWITCH_ENABLE=1` and `PIN_SX1262_RXEN` wiring |
| Erratic TX power | TXEN not driven | Verify `PIN_SX1262_TXEN` wiring |
| Spurious CRC errors at close range | Overdriving receiver (near-field) | Increase distance or reduce TX power: `-DRIVR_TX_POWER_DBM=14` |

For detailed metric-to-symptom mapping see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
