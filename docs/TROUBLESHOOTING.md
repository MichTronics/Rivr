# RIVR Troubleshooting Guide

Maps observed symptoms to `@MET` metric keys and firmware fixes.

Collect a supportpack first to get a snapshot of all metrics:

```bash
# On the serial monitor, type:
supportpack
```

The device prints a `@SUPPORTPACK` JSON block with firmware version, build flags,
all 80 metric counters, and current radio parameters. Attach this output to any
bug report. See [README.md](../README.md#collecting-a-supportpack) for the full procedure.

---

## Quick Symptom Index

| Symptom | Jump to |
|---|---|
| No packets received or transmitted | [§ Radio not working](#radio-not-working) |
| High packet loss / short range | [§ RF link quality](#rf-link-quality) |
| Messages not forwarded across mesh | [§ Routing failures](#routing-failures) |
| Node resets frequently | [§ Watchdog / resets](#watchdog-and-resets) |
| Queue full / messages dropped | [§ Congestion and capacity](#congestion-and-capacity) |
| Airtime budget exhausted | [§ Duty-cycle limit](#duty-cycle-limit) |
| Display blank or shows garbage | [§ Display issues](#display-issues) |
| BLE not connecting / no data over BLE | [§ BLE issues](#ble-issues) |
| OTA program push not accepted | [§ OTA push failures](#ota-push-failures) |
| Build / compile errors | [§ Build problems](#build-problems) |

---

## Radio Not Working

### Symptom: No `rx_*` counters increment after boot

**Check:**
1. `rad_rst` — if > 0 the radio has hard-reset at least once; check BUSY/RESET wiring.
2. `rad_stall` — if rising, the BUSY pin is stuck high before TX.

**Actions:**

| Step | What to do |
|---|---|
| Wiring | Re-check SCK/MOSI/MISO/NSS/BUSY/RESET/DIO1 against the [wiring table](RF_SETUP_EU868.md#5-gpio--spi-wiring-e22-900m30s) |
| Power | Measure 3.3 V under load; E22-900M30S draws up to 200 mA during TX |
| RF switch | Verify `RIVR_RFSWITCH_ENABLE=1` and `PIN_SX1262_RXEN` / `PIN_SX1262_TXEN` are correct |
| Frequency | Check both nodes use the same `RIVR_RF_FREQ_HZ` |
| SIM mode | Ensure `RIVR_SIM_MODE` is **not** set in production builds |

### Symptom: `rad_crc` rising but `rx_decode_fail` = 0

**Meaning:** Frames arrive but fail CRC at the radio layer before the protocol layer sees them.
This is typically a weak-signal issue, not a firmware bug.

**Actions:**
- Reduce distance between nodes, or
- Increase TX power (check EIRP limits — see [RF_SETUP_EU868.md](RF_SETUP_EU868.md#4-antenna)), or
- Move to a higher spreading factor (SF9/SF10) for longer range at cost of throughput.

### Symptom: `rx_decode_fail` > 0

**Meaning:** Frames passed radio CRC but failed the RIVR protocol magic / CRC check.
Likely cause: another LoRa device on the same frequency using a different protocol.

**Actions:**
- Verify `net_id` matches across all nodes (compiled-in `RIVR_NET_ID`).
- Check no other LoRaWAN gateway is overdriving the channel.

---

## RF Link Quality

### Metrics to watch

| `@MET` key | Field name | Normal range | Concern |
|---|---|---|---|
| `rad_crc` | `radio_rx_crc_fail` | 0–5/hour | > 20/hour |
| `rx_fail` | `rx_decode_fail` | 0 | any non-zero |
| `rad_stall` | `radio_busy_stall` | 0 | any non-zero |
| `rad_txfail` | `radio_tx_fail` | 0 | any non-zero |

### RSSI / SNR interpretation

Reported in the neighbor table (`@MET` shows neighbour stats on OLED page 6):

| RSSI | Link quality | Action |
|---|---|---|
| −60 to −80 dBm | Excellent | — |
| −80 to −100 dBm | Good | — |
| −100 to −115 dBm | Marginal | Consider adding a repeater |
| < −115 dBm | Poor | Packet loss likely; reposition |

SNR below −7.5 dB (for SF8) means you are at the noise floor; increase spreading factor.

---

## Routing Failures

### Symptom: Messages don't reach destination

**Metrics to check:**

| `@MET` key | Cause | Fix |
|---|---|---|
| `rx_dup` / `rx_dedupe_drop` | Duplicate relay suppression working correctly | Not a problem; expected |
| `rx_ttl` / `rx_ttl_drop` | TTL expired before reaching destination | Increase `RIVR_PKT_DEFAULT_TTL` (default 7) |
| `ttl_rel` / `drop_ttl_relay` | Relay dropped because forwarded TTL would be 0 | Same as above |
| `no_route` / `drop_no_route` | No unicast route; fallback flood used | Wait for ROUTE_REQ/RPL cycle |
| `fab_drop` | Fabric congestion DROP | Reduce traffic, or tune Fabric thresholds |

### Symptom: Unicast messages pile up but never arrive

Check `pq_drop` and `pq_exp` (pending queue drop / expired).

- `pq_exp` rising: route reply never arrived within 10 s (`PENDING_EXPIRY_MS`).
  Verify the destination node is alive (check its beacon in the neighbour table).
- `pq_drop` rising: pending queue (16 slots) is full.
  Reduce unicast burst rate or increase `PENDING_QUEUE_CAP`.

### Symptom: Route keeps reverting to flood

Route cache entries expire after 120 s (`RCACHE_EXPIRY_MS`). If the destination
stops sending beacons or data frames, the reverse-path route is not refreshed.

**Fix:** Ensure destination node sends `PKT_BEACON` periodically (`timer(30000)` in RIVR program).

---

## Watchdog and Resets

### Metrics to check

| `@MET` key | Field name | Meaning |
|---|---|---|
| `rad_rst` | `radio_hard_reset` | Hard-reset counter (busy_stuck, tx_timeout, spurious_irq triggers) |
| `rst_bkof` | `radio_reset_backoff` | Resets denied by backoff; rising = reset storm |
| `rx_tout` | `radio_rx_timeout` | RX silence > 60 s detected (observability only; no reset) |

### Symptom: `rad_rst` incrementing repeatedly

The radio watchdog (`radio_guard_reset()`) fires when:
- BUSY pin stuck for > 3 consecutive TX attempts → `busy_stuck`
- TX hardware timeout × 3 streak → `tx_timeout`
- 5 spurious DIO1 events in a row → `spurious_irq`

**Diagnosis flow:**

```
rad_stall rising?
  → Yes: BUSY pin wiring issue or SX1262 latch-up (check 3.3 V supply)
  → No
      rad_txfail rising?
        → Yes: TX path issue — check TXEN, SX1262 PaConfig, antenna
        → No
            rx_tout rising?
              → Yes: no packets or IRQs in 60 s — informational only; check traffic level, antenna, and link budget
```

### Symptom: `rst_bkof` rising (reset denied by backoff)

The firmware enforces a 5-second backoff between hard resets to prevent reset storms.
If the underlying hardware fault persists, `rst_bkof` will climb.

**Action:** Power-cycle the node; if problem persists, check 3.3 V supply stability
and SPI wiring for intermittent contact.

---

## Congestion and Capacity

### Symptom: `tx_full` / `tx_queue_full` rising

The TX SPSC queue has 4 slots (cap − 1 = 3 usable). When it fills:
- A fallback flood frame is generated with `PKT_FLAG_FALLBACK` and TTL = 3.
- `tx_queue_full` increments.

**Actions:**
- Check `txq_peak` — if always 3, the queue is perpetually near-full.
- Increase TX drain rate (reduce `FORWARD_JITTER_MAX_MS`) **or** reduce incoming traffic.
- On a repeater: verify Fabric is enabled (`RIVR_FABRIC_REPEATER=1`) to throttle chatty neighbours.

### Symptom: `cls_chat` rising (chat class drops)

The airtime token-bucket is exhausted for `PKT_CHAT` frames.

| Token level | `@MET` indicator | Action |
|---|---|---|
| Approaching low watermark | `at_low` > 0 | Reduce relay throughput |
| Empty — dropping CHAT | `cls_chat` > 0 | Reduce traffic or lower SF |
| Dropping METRICS | `cls_met` > 0 | Same; METRICS drops before CHAT in some scenarios |

### Symptom: `fab_drop` or `fab_delay` non-zero (Fabric suppression)

This is expected behaviour on a congested repeater. Fabric drops/delays only
`PKT_CHAT` and `PKT_DATA`; control frames are never affected.

To reduce Fabric aggressiveness, increase the DROP threshold:

```c
// firmware_core/rivr_fabric.h
#define RIVR_FABRIC_DROP_THRESHOLD   80u   // default; raise to 90 to be less aggressive
#define RIVR_FABRIC_DELAY_THRESHOLD  50u   // default
```

---

## Duty-Cycle Limit

### Symptom: `dc_blk` > 0

The EU868 1 %/hour hard cap is active. Every blocked frame increments `dc_blk`.

**Diagnosis:**

```
Estimate required airtime:
  frames/hour × toa_ms = total_ms

At SF8/BW62.5: CHAT ~184 ms, BEACON ~140 ms
36 000 ms / hour = 1 % budget

100 CHAT/hour × 184 ms = 18 400 ms — fine (0.51%)
500 CHAT/hour × 184 ms = 92 000 ms — exceeds budget
```

**Fixes:**
1. Space out beacons: `timer(60000)` instead of `timer(30000)`.
2. Reduce non-essential data frames.
3. Move to SF7 (≈ 50 ms per CHAT frame) to fit more frames in the budget.
4. Use a second channel (`RIVR_RF_FREQ_HZ`) for a parallel repeater to split load.

---

## BLE Issues

> BLE is only active when `RIVR_FEATURE_BLE=1` is set **and** the `sdkconfig.ble` fragment
> is included in the build (`CONFIG_BT_ENABLED=y`).  When disabled, `ble_*` metrics
> stay at zero and the BLE stack is never initialised.

### Symptom: Device does not advertise after boot

**Check `rivr_ble_init()` reached:**  
Look for `I [ble] BLE init` in the boot log.  If absent, either `RIVR_FEATURE_BLE=0`
or `RIVR_SIM_MODE=1`.

**Check `CONFIG_BT_ENABLED` is set:**  
Build with `sdkconfig.ble` fragment:  
```ini
; platformio.ini
board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble"
build_flags = -DRIVR_FEATURE_BLE=1
```

**Check activation window:**  
BLE is only active during an activation window:
- `BOOT_WINDOW`: first 120 s after boot.
- `BUTTON`: 300 s after trigger (`rivr_ble_activate(RIVR_BLE_MODE_BUTTON)`).
- `APP_REQUESTED`: no timeout; call `rivr_ble_deactivate()` to stop.

If the boot window has expired, the device will have stopped advertising; power-cycle
or call `rivr_ble_activate()` programmatically.

### Symptom: BLE connects but no frames arrive from node

**Check `ble_tx` rising:**  
If `ble_tx` in `@MET` is not incrementing after LoRa frames arrive,
`rivr_ble_service_notify()` is not being called. Verify `rivr_sources.c` wiring:
check that `RIVR_FEATURE_BLE=1` is in your `build_flags`.

**Check SUBSCRIBE:**  
The companion app must subscribe to the TX characteristic (6E400003) NOTIFY before
frames are delivered. Most BLE UART apps do this automatically on connect.

### Symptom: Node receives BLE writes but frames are not injected into mesh

**Check `ble_rx` vs `ble_err`:**

| Metric | Meaning |
|---|---|
| `ble_rx` rising | Frames received and pushed to `rf_rx_ringbuf` successfully |
| `ble_err` rising | Push to `rf_rx_ringbuf` failed — ringbuffer full |

If `ble_err` is rising, the main loop is not draining `rf_rx_ringbuf` fast enough,
or the buffer is overwhelmed by high LoRa traffic. Reduce BLE write rate.

### Symptom: `ble_err` non-zero

| Common cause | Diagnosis |
|---|---|
| `rf_rx_ringbuf` full | `ble_err` increments; reduce BLE TX rate or check main-loop stall (`loop_jitter_ms` high) |
| BLE GATTS notify failures | Check the BLE log for `notify failed` or pairing/auth errors; verify bonding and MTU setup |

---

## Display Issues

### Symptom: OLED blank after boot

1. Check I²C address — SSD1306 auto-detects 0x3C and 0x3D.
2. Check SDA/SCL wiring and pull-up resistors (4.7 kΩ to 3.3 V recommended).
3. Verify `FEATURE_DISPLAY=1` in build flags.
4. Check `rivr_log` for `[DISPLAY] init fail` lines.

### Symptom: OLED shows garbled content after a few hours

The display task runs on CPU1. If the I²C bus glitches (usually power supply noise),
`display.c` triggers a level-2 recovery (re-init SSD1306) after 3 consecutive flush
failures. This should be transparent. If corruption persists, add a 100 µF capacitor
on the 3.3 V rail near the SSD1306.

---

## OTA Push Failures

### Symptom: `PKT_PROG_PUSH` sent but program not reloaded

- `PKT_PROG_PUSH` is **not relayed** — it must be sent directly to the target node
  (within radio range or via unicast with correct `dst_id`).
- The payload must be a null-terminated RIVR source string.
- Parse failures are logged as `[EMBED] parse error` on the serial monitor.
- If NVS write fails, the old program continues running; check NVS partition size in `partitions.csv`.

---

## Build Problems

### Symptom: Linker error `undefined reference to rivr_*`

The Rust static library is missing. Build it first:

```bash
cd rivr_core
cargo build --features ffi --release
# then re-run pio
```

### Symptom: `error: target 'xtensa-esp32-espidf' not found`

Install the Xtensa Rust toolchain:

```bash
espup install
# then:
cargo +esp build --target xtensa-esp32-espidf --features ffi --release
```

### Symptom: Size exceeds IRAM/FLASH limit

Check `platformio.ini` for `board_upload.flash_size` and `partitions.csv`.
Ensure `sdkconfig.defaults` has `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`.

---

## Collecting a Supportpack

A supportpack bundles everything needed to diagnose issues offline.

```bash
# 1. Open serial monitor
~/.platformio/penv/bin/pio device monitor -e client_esp32devkit_e22_900

# 2. Type the command (client builds only)
> supportpack

# 3. Copy the single @SUPPORTPACK line
@SUPPORTPACK {"env":"client_esp32devkit_e22_900","sha":"659b981","built":"Feb 27 2026 14:05:32","role":"client","radio":"SX1262","freq":869480000,"sf":8,"bw_khz":125,"cr":"4/8","fabric":0,"sim":0,"uptime_ms":12345,"rx_frames":42,"tx_frames":18,"routing":{"neighbors":2,"routes":3,"pending":0},"dc":{"remaining_us":35946200,"used_us":53800,"blocked":0},"met":{"rx_fail":0,"rx_dup":1,...}}
```

**Repeater builds** (no interactive CLI) emit `@SUPPORTPACK` automatically every 30 seconds.
Capture it with:

```bash
~/.platformio/penv/bin/pio device monitor \
  -e repeater_esp32devkit_e22_900 | grep @SUPPORTPACK | head -1
```

Include this output with every bug report. See
[GitHub issue template](.github/ISSUE_TEMPLATE/bug_report.md) for the full format.
