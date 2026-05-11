# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## v0.2.1 — 2026-05-11

### Fixed

- **Heltec LoRa32 V4 OLED blank screen**: `PIN_VEXT_EN` was documented in
  `variants/heltec_lora32_v4/config.h` comments but never `#define`'d.
  `platform_init()` never drove GPIO 36 low, leaving the OLED peripheral rail
  unpowered. Fixed by adding `#define PIN_VEXT_EN 36` to the variant config.

- **SX1276 driver build error** (`client_lilygo_lora32_v21`): `vTaskDelay` and
  `pdMS_TO_TICKS` caused implicit-declaration errors because `radio_sx1276.c`
  was missing `freertos/FreeRTOS.h` and `freertos/task.h` includes.

### Versions

| Component | Version |
|-----------|---------|
| Firmware (`RIVR_VERSION_STR`) | `0.2.1` |

---

## v0.2.0 — 2026-04-26

### Added

- **send_queue outbox** (`firmware_core/send_queue.c/.h`): 16-slot FIFO for
  originated messages. Frames are held in BSS and drained into `rf_tx_queue`
  each tick, decoupling the CLI from the radio TX ring-buffer. Entries expire
  after 120 s if never transmitted (duty-cycle saturation). Metrics counters
  (`sq_dropped`, `sq_expired`, `sq_peak`) added to `rivr_metrics_t`.

- **nRF52840 / RP2040 build support for send_queue and sensors**: added
  `send_queue.c` and `sensors.c` to `build_src_filter` in all four non-ESP32
  PlatformIO variant configs (`heltec_t114`, `rak4631`, `seeed_t1000_e`,
  `waveshare_rp2040_lora_hf`). Defined `g_send_queue`, `send_queue_init()`,
  and `send_queue_tick()` in `main_nrf52.cpp` and `main_rp2040.cpp`.

- **Companion app database code generation**: `drift` / `drift_flutter` /
  `drift_dev` / `build_runner` were already in `pubspec.yaml`; ran
  `dart run build_runner build` to produce `app_database.g.dart` — resolves
  all `undefined_identifier` errors from the missing generated file.

### Changed

- **CLI send path unthrottled**: removed `rivr_policy_allow_origination()` gate
  from `cli_enqueue_chat()` and `cli_enqueue_chan_chat()` in `rivr_cli.c`.
  Rate-limiting is now handled exclusively by `send_queue` and the duty-cycle
  subsystem, so rapid multi-message sends from the USB CLI no longer stall after
  the first frame.

- **Chat relay throttle restored to 2000 ms** (`RIVR_PARAM_CHAT_THROTTLE_MS`):
  was incorrectly set to `0UL` in the previous cycle, which caused
  `rivr_engine_init` to fail with `RIVR_ERR_PARSE` (code 3) at boot because
  `throttle.ms(0)` is invalid Rivr DSL syntax.

### Fixed

- **Boot parse error** (`RIVR_ERR_PARSE` / code 3): caused by
  `RIVR_PARAM_CHAT_THROTTLE_MS = 0UL` generating `throttle.ms(0)` in the
  compiled Rivr program. Fixed by restoring the throttle to `2000UL`; CLI
  origination is now ungated at the policy layer.

- **CI policy test (test_policy.c test 3)**: TTL-clamping test for `PKT_CHAT`
  used TTL=7 which is below `POLICY_TTL_CHAT` cap of 12, so no clamp occurred
  and the assertion failed. Fixed by using TTL=15 (> cap=12).

- **nRF52 linker error** (`undefined reference to g_send_queue`): `g_send_queue`
  was only defined in `firmware_core/main.c` (ESP32 entry point). Fixed by
  defining it in `main_nrf52.cpp` (shared by `heltec_t114`, `rak4631`,
  `seeed_t1000_e`) and `main_rp2040.cpp`.

- **Flutter analyze — 22 issues resolved**:
  - Removed unused local variable `canUseBle` (`settings_screen.dart`)
  - Renamed `_txOptions` / `_tempOptions` / `_rhOptions` to drop leading
    underscores (`settings_screen.dart`)
  - Migrated deprecated `Radio(groupValue/onChanged)` to `RadioGroup` ancestor
    pattern (`settings_screen.dart`)
  - Replaced deprecated `desiredAccuracy:` with
    `LocationSettings(accuracy: ...)` at two call-sites (`settings_screen.dart`)
  - Fixed `getDotPainter` lambda wildcard params (`diagnostics_screen.dart`,
    `sensor_channel_screen.dart`)
  - Added curly braces to bare `if` statements (`diagnostics_screen.dart`)
  - Fixed angle-bracket HTML warning in doc comment (`app_settings.dart`)
  - Renamed single-char wildcard lambda params across multiple screens to
    satisfy `unnecessary_underscores` / `no_leading_underscores_for_local_identifiers`

### Versions

| Component | Version |
|-----------|---------|
| Firmware (`RIVR_VERSION_STR`) | `0.2.0` |
| `rivr_core` (Rust crate) | `0.2.0` |
| `rivr_host` (Rust crate) | `0.2.0` |
| Rivr Companion app | `0.2.0+2` |
| VS Code extension | `0.2.0` |
| Website | `0.2.0` |

---

## v0.1.0 — initial release

- Initial public release.
