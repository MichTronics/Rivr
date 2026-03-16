# Contributing to Rivr

Thank you for your interest in contributing to Rivr!  This document describes how
to report issues, propose changes, and submit pull requests.

---

## Before you start

- Read [docs/en/architecture.md](docs/en/architecture.md) to understand the system design.
- Read [docs/quickstart.md](docs/quickstart.md) to get the firmware running on your hardware.
- Check the open issues before opening a new one — your question or bug may already be tracked.

---

## Reporting bugs

1. Reproduce the issue on the latest `main` branch.
2. Open a GitHub Issue using the **Bug Report** template.
3. Attach a `@SUPPORTPACK` dump from the serial monitor:
   ```
   rivr> supportpack
   @SUPPORTPACK {"fw":"...", "git":"...", ...}
   ```
4. Include your board type, firmware environment name, and any radio
   settings you changed from the defaults.

---

## Suggesting features

Open a GitHub Issue using the **Feature Request** template, or start a
discussion in the **GitHub Discussions** tab.

Please do not open a pull request for a new feature without first discussing
it in an issue.  This saves time for both you and the maintainers.

---

## Pull request guidelines

### Scope

- Bug fixes and test coverage improvements are always welcome.
- Documentation improvements are always welcome.
- New features must be discussed and approved via an issue first.
- Do NOT restructure the existing firmware layout without prior discussion.

### Size

Keep pull requests small and focused.  A PR that changes one thing is easier
to review than a PR that changes ten things.  If your change is large, break
it into independent sequential PRs.

### Code style

- **C firmware:** follow the existing coding conventions in `firmware_core/`:
  - 4-space indent, K&R brace style, `snake_case` identifiers
  - Keep functions well under 100 lines; internal helpers are `static`
  - Every public function in a `.h` file must have a Doxygen `/** ... */`
    block comment
  - No dynamic allocation (`malloc`/`new`) after boot — this is a hard rule
  - All new counters go in `rivr_metrics.h` using `g_rivr_metrics.<name>++`
- **Rust (`rivr_core/`):** run `cargo fmt` and `cargo clippy -- -D warnings`
  before submitting
- **Python tools (`tools/`):** PEP 8; use only the standard library unless
  there is a compelling reason for an external dependency

### Tests

- All C changes must pass `make -C tests all` (currently 204 tests, 0 failures)
- All Rust changes must pass `cargo test --workspace`
- Add new test cases for any new or fixed behavior

### CI check

Push your branch and ensure all GitHub Actions jobs pass:
- `rust-check` — fmt + clippy + cargo test
- `c-tests` — GCC build + full test suite
- `asan-tests` — AddressSanitizer + UBSan
- `size-check` — binary size regression gate (±5 %)
- `determinism` — reproducible build check

---

## Local build instructions

See [docs/quickstart.md](docs/quickstart.md) for the full setup guide.

```bash
# Host simulation (Rust)
cargo build -p rivr_host
cargo run -p rivr_host -- tests/rivr_replay.jsonl

# C test suite
make -C tests all

# Firmware (PlatformIO)
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900

# Rust lint
cargo clippy --workspace -- -D warnings
cargo fmt --check --all
```

---

## Project structure overview

```
firmware_core/     C firmware — drivers, routing, protocol, metrics, CLI
rivr_core/src/     Rust library — RIVR DSL parser, compiler, engine (no_std)
rivr_host/src/     Rust host tools — rivrc compiler CLI, replay harness
rivr_layer/        C glue between Rust engine and firmware
variants/          Board-specific pin config and PlatformIO environments
tools/             Host utilities — decode, monitor, mesh-map
docs/              Documentation (English + Dutch)
tests/             C test suites (acceptance, routing, replay, OTA, …)
```

---

## Developer Certificate of Origin

By submitting a pull request you certify that:

1. The contribution was created in whole or in part by you and you have the
   right to submit it under the project's MIT license; or
2. The contribution is based upon previous work that, to the best of your
   knowledge, is covered under an appropriate open source license and you have
   the right, under that license, to submit that work with modifications under
   the MIT license.

---

## Questions?

Open a Discussion on GitHub or include `@mentions` in your issue.
