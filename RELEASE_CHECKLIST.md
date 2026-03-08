# RIVR Release Checklist

Use this checklist for every tagged release. Complete all items before pushing the tag.

---

## 1. Code Quality

- [ ] `make -C tests` passes — all suites green (acceptance + recovery + replay)
  ```bash
  make -C tests
  # Expected: PASS: 204  FAIL: 0
  ```
- [ ] No new `-Wall -Wextra` warnings introduced
  ```bash
  cd tests && make 2>&1 | grep -E "warning:|error:"
  ```
- [ ] `cargo test` passes for `rivr_core` (host features)
  ```bash
  cd rivr_core && cargo test --features std
  ```
- [ ] `cargo clippy` clean
  ```bash
  cd rivr_core && cargo clippy --features std -- -D warnings
  ```

## 2. Firmware Builds

- [ ] `repeater_esp32devkit_e22_900` builds without error
  ```bash
  ~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900
  ```
- [ ] `client_esp32devkit_e22_900` builds without error
  ```bash
  ~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900
  ```
- [ ] `repeater_lilygo_lora32_v21` builds without error
  ```bash
  ~/.platformio/penv/bin/pio run -e repeater_lilygo_lora32_v21
  ```
- [ ] Simulation mode builds and runs 8 rounds without assertion failures
  ```bash
  ~/.platformio/penv/bin/pio run -e esp32_sim -t upload && \
  ~/.platformio/penv/bin/pio device monitor -e esp32_sim | grep -E "SIM|PASS|FAIL"
  ```

## 3. Hardware Smoke Test

Run on at least two physical nodes (one repeater + one client):

- [ ] Both nodes boot without `rad_rst` on first power-on
- [ ] Repeater OLED shows all 7 pages correctly
- [ ] Client serial monitor accepts `chat`, `id`, `help` commands
- [ ] Client CHAT message received by repeater (check OLED page 2 rx count)
- [ ] Repeater CHAT forwarded and received by second client
- [ ] `supportpack` command returns valid JSON on both nodes
- [ ] `@MET` shows `rad_crc` = 0 and `rx_fail` = 0 at 5 m range
- [ ] After 10 minutes: `dc_blk` = 0 under normal chat traffic

## 4. Metrics Baseline

Run the following and paste output in the release notes:

```bash
# On repeater after 10 min of idle:
echo "supportpack" | pio device monitor -e repeater_esp32devkit_e22_900 | grep @SUPPORTPACK
```

Expected healthy baseline:

| `@MET` key | Expected |
|---|---|
| `rad_stall` | 0 |
| `rad_txfail` | 0 |
| `rad_crc` | 0–2 |
| `rx_fail` | 0 |
| `dc_blk` | 0 |
| `fab_drop` | 0 (idle) |
| `pq_exp` | 0 |
| `rst_bkof` | 0 |

## 5. Documentation

- [ ] `README.md` version/date updated
- [ ] `docs/ARCHITECTURE.md` diagrams reflect current code
- [ ] `docs/ROLES.md` flag table matches `platformio.ini` envs
- [ ] `docs/RF_SETUP_EU868.md` constants match `airtime_sched.h` / `dutycycle.c`
- [ ] `docs/TROUBLESHOOTING.md` metric keys match `rivr_metrics.c`
- [ ] `RELEASE_CHECKLIST.md` test counts updated if new tests added
- [ ] Dutch docs (`docs/nl/`) updated for any user-facing changes

## 6. Version Bump

- [ ] Update version string in `firmware_core/build_info.c` (or `build_info.h`)
  ```c
  #define RIVR_VERSION_STR  "1.x.y"
  ```
- [ ] Update `rivr_core/Cargo.toml` version if Rust API changed
- [ ] Update `rivr_host/Cargo.toml` version to match
- [ ] Commit version bump:
  ```bash
  git add firmware_core/build_info.c rivr_core/Cargo.toml rivr_host/Cargo.toml
  git commit -m "chore: bump version to 1.x.y"
  ```

## 7. Tagging and Release Notes

- [ ] Tag the release:
  ```bash
  git tag -a v1.x.y -m "Release 1.x.y — <one-line summary>"
  git push origin v1.x.y
  ```
- [ ] GitHub release notes include:
  - [ ] Summary of changes (link to commits since previous tag)
  - [ ] Updated metric baseline (from § 4)
  - [ ] Known limitations (copy from README Known Limitations section)
  - [ ] Flash size for each environment (from PlatformIO build output)
  - [ ] Supportpack format version (if changed)

## 8. Post-Release

- [ ] Monitor issue tracker for 48 hours after release
- [ ] If a critical regression is found, tag a `.1` patch within 24 hours
- [ ] Update `origin/main` if tag was on a release branch

---

## Flash Size Reference (update each release)

| Environment | Flash used | IRAM used |
|---|---|---|
| `repeater_esp32devkit_e22_900` | | |
| `client_esp32devkit_e22_900` | | |
| `repeater_lilygo_lora32_v21` | | |
