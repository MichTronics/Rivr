---
name: Hardware compatibility
about: Testing RIVR on a new board or radio module
title: "hw: <board/module name>"
labels: ["hardware", "triage"]
assignees: []
---

## Hardware being tested

| Field | Value |
|---|---|
| MCU / Board | |
| Radio chip | e.g. SX1262, SX1276, SX1278 |
| Module | e.g. EBYTE E22-900M30S, Ra-02, RFM95W |
| Frequency band | |
| PA / antenna switch | e.g. none, internal (E22), external SKY65383 |
| 3.3 V supply (mA available) | |
| TCXO present? | Yes / No |

## Pin mapping used

| Signal | GPIO |
|---|---|
| SCK | |
| MOSI | |
| MISO | |
| NSS | |
| BUSY | |
| RESET | |
| DIO1 | |
| RXEN | |
| TXEN | |

## Build flags

```ini
build_flags =
    -DRIVR_RF_FREQ_HZ=
    -DRIVR_RADIO_SX1262=1   ; or SX1276
    -DPIN_SX1262_NSS=
    ; ... other overrides
```

## Test results

<!-- Describe what works and what does not. -->

### Basic RX/TX

- [ ] Node boots without `rad_rst` immediately
- [ ] `supportpack` prints without hanging
- [ ] TX frame appears on air (verified with SDR or second node / RSSI)
- [ ] RX frame received from a known-good RIVR node
- [ ] CRC pass rate > 95 % at 1 m distance

### Supportpack output

```json
@SUPPORTPACK { ... }
```

## Known issues / workarounds

<!-- Describe anything special needed for this hardware. -->
