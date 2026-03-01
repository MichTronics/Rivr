# RIVR Release Checklist

This document describes the steps required to cut a tagged release of RIVR.
Follow every section in order; do not skip steps on "minor" releases.

---

## 1. Pre-release validation

### 1.1 Full test suite (C + Rust)

```bash
# All C test suites (acceptance, recovery, replay, dutycycle, policy, OTA)
make -C tests test

# AddressSanitizer / UBSanitizer pass
make -C tests asan

# Rust unit tests
cargo test --workspace
```

All passes must be **0 FAIL**.  Any failure blocks the release.

### 1.2 Binary size regression check

```bash
make -C tests size_check
```

The command compares the sizes of all test binaries against the stored
baselines in `tests/size_baselines.txt`.  Any binary that grew by more than
**512 bytes** fails the gate.  To update baselines after an intentional change:

```bash
make -C tests update_baselines
git add tests/size_baselines.txt
git commit -m "chore: update size baselines for vX.Y.Z"
```

### 1.3 Determinism check

```bash
make -C tests determinism_check
```

Rebuilds each test binary twice and verifies that the SHA-256 hashes are
identical.  Any mismatch indicates a non-deterministic build input (timestamp
macro, random padding, etc.) and must be resolved before releasing.

### 1.4 Variant compile check

```bash
make -C tests variant_check
```

Verifies that every board variant in `variants/` compiles cleanly.  This
catches config-header-only breakage that the main build might miss.

### 1.5 Fuzzing (optional but recommended)

Run the coverage-guided fuzz targets for at least 30 minutes each before a
major release:

```bash
# Rust fuzz targets
cargo +nightly fuzz run dsl_parse   -- -max_total_time=1800
cargo +nightly fuzz run compiler    -- -max_total_time=1800
cargo +nightly fuzz run ffi_entrypoints -- -max_total_time=1800

# C protocol + routing harness (AFL++)
make -C tests fuzz_harness
afl-fuzz -i tests/fuzz_seeds -o /tmp/fuzz_out -- tests/fuzz_ffi_harness @@
```

Any crash that is not already in the known-corpus must be fixed before
the release tag is created.

---

## 2. Version bump

### 2.1 Version constants

Update the version in each location below (search for `RIVR_VERSION` or
the hard-coded string):

| File | Symbol / key | Notes |
|------|-------------|-------|
| `firmware_core/main.c` | `RIVR_VERSION_STR` | Shown on OLED boot screen |
| `rivr_core/Cargo.toml` | `version = "..."` | Rust crate version |
| `rivr_host/Cargo.toml` | `version = "..."` | Host tools version |
| `Cargo.toml` (workspace) | `version = "..."` if present | Workspace root |
| `tools/vscode-rivr/package.json` | `"version": "..."` | VS Code extension |

### 2.2 Changelog

Add an entry at the top of `CHANGELOG.md` (create if absent):

```markdown
## vX.Y.Z — YYYY-MM-DD

### Added
- …

### Changed
- …

### Fixed
- …
```

### 2.3 Commit

```bash
git add -u
git commit -m "chore(release): bump version to vX.Y.Z"
```

---

## 3. OTA key management (production releases only)

### 3.1 Key ring status

Verify the active signing keys in `firmware_core/rivr_pubkey.h`:

```c
#define RIVR_OTA_KEY_COUNT 2u
static const uint8_t RIVR_OTA_KEYS[RIVR_OTA_KEY_COUNT][32] = { … };
```

Ensure at least one key remains valid after any planned rotation.
The firmware verifies `key_id < RIVR_OTA_KEY_COUNT`, so adding a new key
**before** retiring the old one is safe.

### 3.2 Sign an OTA payload

```bash
# Build a program payload:
#   sig[64] | key_id[1] | seq[4] | program_text
#
# Use the rivr_sign tool (or any Ed25519 implementation):
#   rivr_sign --key key0.pem --key-id 0 --seq <NEXT_SEQ> program.rivr > payload.bin

# Verify the payload before pushing:
#   ./tools/rivr_decode payload.bin | python3 -c "import sys,json; d=json.load(sys.stdin); assert d['ota']['key_id']==0"
```

The sequence number (`seq`) **must** be strictly greater than the last
accepted sequence stored in device NVS.  There is no recovery for a
skipped sequence — only a firmware reflash can reset the counter.

### 3.3 Boot confirmation

After the new program pushes successfully, the device sets `ota_pending=1`.
Ensure the newly-running RIVR program calls (or causes the host app to call)
`rivr_ota_confirm()` within the first boot.  If the device reboots before
confirmation, the previous program is restored from NVS.

---

## 4. Firmware build (ESP-IDF / PlatformIO)

Build all four production environments and capture the `.bin` artefacts:

```bash
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900
~/.platformio/penv/bin/pio run -e repeater_esp32devkit_e22_900
~/.platformio/penv/bin/pio run -e client_lilygo_lora32_v21
~/.platformio/penv/bin/pio run -e repeater_lilygo_lora32_v21
```

Confirm build outputs are present and sizes are within expected bounds:

| Environment | Binary | Max size |
|-------------|--------|----------|
| `client_esp32devkit_e22_900` | `.pio/build/.../firmware.bin` | 1 536 KB |
| `repeater_esp32devkit_e22_900` | `.pio/build/.../firmware.bin` | 1 536 KB |
| `client_lilygo_lora32_v21` | `.pio/build/.../firmware.bin` | 1 536 KB |
| `repeater_lilygo_lora32_v21` | `.pio/build/.../firmware.bin` | 1 536 KB |

---

## 5. Host tooling build

```bash
# release build
cargo build --release -p rivr_host

# verify the compiler CLI works
cargo run --release -p rivr_host --bin rivrc -- --help

# verify the decoder tool builds
make -C tools
./tools/rivr_decode --help
```

---

## 6. Tag and push

```bash
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin main --tags
```

The CI pipeline (`.github/workflows/ci.yml`) will run all gates again on
the tag commit.  The release is considered stable only when all CI jobs pass
on the tag.

---

## 7. GitHub Release

1. Draft a new Release on GitHub, targeting the new tag.
2. Copy the `CHANGELOG.md` entry for this version into the release body.
3. Attach the four firmware `.bin` artefacts.
4. Attach the `rivrc` host tool binary (Linux x86_64 static build if available).
5. Publish.

---

## 8. Post-release

- Update `docs/en/overview.md` version badge / "latest release" line.
- Announce on the project forum / mailing list.
- Open a new milestone for the next release cycle.
- Rotate signing keys if this release introduced new hardware keys
  (append new key first, deploy, then remove old key in the next release).
