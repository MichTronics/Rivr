# Rivr Beta Release Checklist

Use this checklist before tagging any public beta release (`vX.Y.Z-beta.N`).
All items must be ✅ before pushing the tag.

---

## 1. Test suite

- [ ] `make -C tests all` passes: `PASS: 204   FAIL: 0`
- [ ] `make -C tests asan` passes (ASan + UBSan — no memory errors or UB)
- [ ] `cargo test -p rivr_core` passes — all Rust unit tests green
- [ ] `cargo test -p rivr_host` passes — replay + compiler CLI tests green
- [ ] `cargo clippy --workspace -- -D warnings` is clean
- [ ] `cargo fmt --check --all` is clean

## 2. CI pipeline

- [ ] GitHub Actions `ci.yml` is green on `main` (all jobs: rust-check, c-tests, asan-tests, size-check, determinism)
- [ ] No skipped or manually-bypassed CI jobs
- [ ] CI badge in README.md points to the correct repository URL (not `YOUR_ORG/YOUR_REPO`)

## 3. Firmware builds

- [ ] `pio run -e client_esp32devkit_e22_900` — builds without warnings
- [ ] `pio run -e repeater_esp32devkit_e22_900` — builds without warnings
- [ ] `pio run -e client_lilygo_lora32_v21` — builds without warnings
- [ ] `pio run -e repeater_lilygo_lora32_v21` — builds without warnings
- [ ] Binary sizes have not regressed beyond the 5% gate: `make -C tests size_check`
- [ ] Build is deterministic: `make -C tests determinism_check`

## 4. Hardware smoke test

- [ ] Flash `client_esp32devkit_e22_900` to a physical ESP32 DevKit + E22 board
- [ ] Node boots, prints startup banner, shows `rivr>` prompt at 115200 baud
- [ ] `id` command responds with correct role and firmware version
- [ ] `neighbors` command responds (may show empty table if no peer present)
- [ ] `chat test` successfully transmits (`@CHT tx` log line visible)
- [ ] `supportpack` command emits a well-formed `@SUPPORTPACK` JSON block

## 5. Documentation

- [ ] [docs/quickstart.md](quickstart.md) is accurate for the current firmware version
- [ ] [docs/en/build-guide.md](en/build-guide.md) wiring tables are up to date
- [ ] [docs/en/language-reference.md](en/language-reference.md) reflects any DSL changes
- [ ] [docs/releasing.md](releasing.md) release procedure matches actual Makefile targets
- [ ] README.md hardware matrix is accurate (no unreleased boards listed as Supported)
- [ ] README.md CI badge URL is updated to the real repository path

## 6. Repository hygiene

- [ ] No `your-org` or `YOUR_ORG/YOUR_REPO` placeholder strings remain in published files
  ```bash
  grep -r "your-org\|YOUR_ORG" --include="*.md" --include="*.yml" --include="*.yaml" .
  # Expected: zero results
  ```
- [ ] RELEASE_CHECKLIST.md test count matches actual passing test count
- [ ] No accidental debug prints (`printf`/`ESP_LOGI` marked `DBG:`) left in firmware_core
- [ ] `git log --oneline -10` shows clean, descriptive commit messages (no "WIP" or "fixup")

## 7. Security

- [ ] No private keys, PSKs, or credentials are committed (`git grep -i "key\|secret\|password"`)
- [ ] `rivr_pubkey.h` contains the intended public key for OTA signature verification
- [ ] HMAC PSK in test stub (`tests/test_stubs.c`) is clearly marked as test-only

## 8. Release artefacts

- [ ] Tag created: `git tag -a vX.Y.Z-beta.N -m "Rivr vX.Y.Z-beta.N"`
- [ ] GitHub Release draft created with:
  - [ ] Summary of changes (link to commits since last tag)
  - [ ] Known limitations for this beta
  - [ ] `@SUPPORTPACK` instructions in the bug report template
- [ ] Release notes mention any wire-protocol changes that break compatibility with previous beta
- [ ] Milestone closed (if applicable)

---

## Quick verification commands

```bash
# Full host test suite
make -C tests all

# Rust checks
cargo test --workspace && cargo clippy --workspace -- -D warnings && cargo fmt --check --all

# Check for leftover placeholders
grep -r "your-org\|YOUR_ORG" --include="*.md" --include="*.yml" .

# Size and determinism gates
make -C tests size_check
make -C tests determinism_check
```
