# Rivr Radio Layer

This document explains the radio hardware support, air interface parameters,
duty-cycle enforcement, and recovery mechanisms in Rivr firmware.

For full hardware wiring tables see [docs/en/build-guide.md](en/build-guide.md).

---

## Supported radio hardware

| Radio chip | Module | Driver | Environments |
|---|---|---|---|
| **SX1262** | EBYTE E22-900M30S / E22-900M33S | `radio_sx1262.c` | `esp32devkit_e22_900_*` |
| **SX1276** | LilyGo LoRa32 v2.1 built-in | `radio_sx1276.c` | `lilygo_lora32_v21_*` |

The radio chip is selected at compile time by the variant `config.h`:

```c
#define RIVR_RADIO_SX1262  1   // SX1262 path
// or
#define RIVR_RADIO_SX1276  1   // SX1276 path
```

---

## Default air parameters

All nodes on the same mesh **must use identical air parameters**.

| Parameter | Default value | Configuration macro |
|---|---|---|
| Frequency | 869.480 MHz | `RIVR_RF_FREQ_HZ` |
| Spreading factor | SF8 | `RF_SPREADING_FACTOR` |
| Bandwidth | 62.5 kHz (`62500`) | `RF_BANDWIDTH_HZ` |
| Coding rate | CR 4/8 | `RF_CODING_RATE` |
| Preamble | 8 symbols | (fixed) |
| TX power (chip) | +5 dBm | `RF_TX_POWER_DBM` |

Change radio parameters in your variant's `platformio.ini` (shared `[rf_e22_900]` section):

```ini
[rf_e22_900]
build_flags =
    -DRF_SPREADING_FACTOR=8
    -DRF_BANDWIDTH_HZ=62500
    -DRF_CODING_RATE=8
    -DRF_TX_POWER_DBM=5
    -DRIVR_RF_FREQ_HZ=869480000
```

### Frequency selection guide

| Region | Frequency | Macro |
|---|---|---|
| EU868 g3 (default) | 869.480 MHz | `-DRIVR_RF_FREQ_HZ=869480000` |
| EU868 channel 0 | 868.100 MHz | `-DRIVR_RF_FREQ_HZ=868100000` |
| AU915 / US915 | 915.000 MHz | `-DRIVR_RF_FREQ_HZ=915000000` |
| AS923 | 923.000 MHz | `-DRIVR_RF_FREQ_HZ=923000000` |

> **Regulatory notice:** You are responsible for compliance with the radio
> regulations in your jurisdiction.  Check the permitted frequencies, maximum
> power levels, and duty-cycle limits before transmitting.

---

## Time-on-air

Time-on-air (ToA) is the duration a frame occupies the air. With the primary
E22 preset (SF8, BW 62.5 kHz, CR 4/8) a maximum-length Rivr frame (256 bytes)
takes approximately **760 ms**.

ToA determines:
- How much duty-cycle budget a frame consumes
- The minimum jitter delay to prevent relay storm collisions
- The ACK timeout window for unicast retries

---

## Duty-cycle enforcement

Rivr firmware enforces the EU868 g3 10% duty-cycle budget out of the box.

### Mechanism

A **1-hour sliding window** with 512 slots tracks the cumulative time-on-air
of all transmitted frames.  Before each TX attempt:

1. `dutycycle_check()` is called with the frame's pre-computed ToA.
2. If `used_us + toa_us > DC_BUDGET_US` (360 ms/hr), the frame is **blocked**
   and `duty_blocked` is incremented.
3. After successful TX, `dutycycle_record()` adds the actual ToA to the window.

Entries age out of the window when they are older than 1 hour; the budget is
automatically restored as old transmissions expire.

### Monitoring

```
rivr> metrics
@MET {"dc_pct":6, "rf_airtime_ms_total":218, "rf_duty_blocked_total":0, ...}
```

`dc_pct` is the percentage of the budget consumed in the current window.
`rf_duty_blocked_total` counts frames that were dropped by the gate.

### Changing the budget

Override `DC_DUTY_PCT_X10` in `platformio.ini`:

```ini
build_flags =
    -DDC_DUTY_PCT_X10=10    ; 1 %  (EU868 g1)
    -DDC_DUTY_PCT_X10=100   ; 10 % (EU868 g3, default)
```

---

## SX1262 driver architecture

The SX1262 driver (`radio_sx1262.c`) runs in interrupt-driven mode:

1. **TX path:** `radio_transmit()` writes the frame to the SX1262 FIFO, starts
   TX mode, and polls the DIO1 interrupt pin for `IRQ_TX_DONE`.
2. **RX path:** `radio_start_rx()` puts the SX1262 in continuous RX mode.
   On DIO1 interrupt (RxDone), the ISR reads the frame from the FIFO and
   pushes it to the `rf_rx_ringbuf` ring buffer.
3. **Main loop:** `rivr_tick()` drains `rf_rx_ringbuf` and processes frames.

### BUSY pin handling

The SX1262 BUSY pin must be high before any SPI command.  The driver polls
BUSY with a 10 ms timeout before every SPI write.  If BUSY is stuck:

- 3 consecutive BUSY-stuck events trigger `rivr_panic_check()` radio reset
- `radio_busy_stall` counter is incremented on each wait
- `radio_reset_busy_stuck` counter tracks guard resets

---

## SX1276 driver

The SX1276 driver (`radio_sx1276.c`) follows the same ring-buffer + ISR
architecture as the SX1262 driver, adapted for the SX1276 register map.

The SX1276 uses DIO0 for RxDone/TxDone (wired to `PIN_SX1262_DIO1` slot in
the LilyGo variant's `config.h`).

---

## Recovery and guard mechanisms

The firmware includes automatic radio recovery to handle hardware faults:

| Fault | Detection | Recovery |
|---|---|---|
| BUSY pin stuck | 3 consecutive `wait_busy()` failures | Full SX1262 re-init |
| TX timeout | HW IRQ Timeout flag | Re-init + backoff |
| TX deadline exceeded | SW poll 2×ToA + 100 ms | Re-init |
| RX silence > 60 s | Watchdog in main loop | `radio_start_rx()` restart |
| Spurious DIO1 IRQ | 5 consecutive no-frame reads | Re-init |

All recovery events are counted:

```
rivr> metrics
@MET {"radio_hard_reset":0, "radio_reset_busy_stuck":0,
      "radio_reset_tx_timeout":0, "radio_rx_timeout":0, ...}
```

---

## Airtime scheduler

The **airtime scheduler** (`airtime_sched.c`) provides per-class token buckets
on top of the duty-cycle gate.  Four traffic classes with separate budgets:

| Class | Types | Priority |
|---|---|---|
| CONTROL | BEACON, ROUTE_REQ/RPL, ACK | High |
| CHAT | PKT_CHAT | Medium |
| METRICS | PKT_TELEMETRY | Low |
| BULK | Large data payloads | Lowest |

Token bucket drops are counted per class (`class_drops_ctrl`, `class_drops_chat`, etc.).

---

## SPI pin mapping

### ESP32 DevKit + EBYTE E22-900M30S

| Signal | GPIO |
|---|---|
| SCK | 18 |
| MOSI | 23 |
| MISO | 19 |
| NSS (CS) | 5 |
| BUSY | 32 |
| RESET | 25 |
| DIO1 | 33 |
| RXEN | 21 |
| TXEN | 22 |

### LilyGo LoRa32 v2.1

| Signal | GPIO |
|---|---|
| SCK | 5 |
| MOSI | 27 |
| MISO | 19 |
| NSS (CS) | 18 |
| RESET | 23 |
| DIO0 (→ DIO1 slot) | 26 |

Full wiring guide: [docs/en/build-guide.md](en/build-guide.md)

---

## Further reading

- [docs/en/architecture.md](en/architecture.md) — full system architecture
- [docs/en/build-guide.md](en/build-guide.md) — hardware wiring and antenna notes
- [docs/routing.md](routing.md) — mesh routing documentation
- [docs/RF_SETUP_EU868.md](RF_SETUP_EU868.md) — EU868 regulatory compliance
