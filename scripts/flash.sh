#!/usr/bin/env bash
# scripts/flash.sh — Flash a Rivr firmware environment to a connected board.
#
# Usage:
#   ./scripts/flash.sh                              # flash default env (esp32_sim)
#   ./scripts/flash.sh client_esp32devkit_e22_900   # flash specific env
#   ./scripts/flash.sh repeater_esp32devkit_e22_900
#   ./scripts/flash.sh client_lilygo_lora32_v21
#   ./scripts/flash.sh repeater_lilygo_lora32_v21
#
# The script automatically locates the PlatformIO CLI.

set -euo pipefail

# ── Locate PlatformIO ─────────────────────────────────────────────────────
PIO=""
for candidate in \
    "${HOME}/.platformio/penv/bin/pio" \
    "$(which pio 2>/dev/null || true)" \
    "/usr/local/bin/pio"; do
    if [[ -x "$candidate" ]]; then
        PIO="$candidate"
        break
    fi
done

if [[ -z "$PIO" ]]; then
    echo "ERROR: PlatformIO CLI (pio) not found."
    echo "Install it with:  pip install platformio"
    exit 1
fi

# ── Choose environment ────────────────────────────────────────────────────
ENV="${1:-esp32_sim}"

# Print supported environments if an unknown one is given
SUPPORTED=(
    "esp32_sim"
    "esp32_hw"
    "client_esp32devkit_e22_900"
    "repeater_esp32devkit_e22_900"
    "client_lilygo_lora32_v21"
    "repeater_lilygo_lora32_v21"
)

valid=0
for s in "${SUPPORTED[@]}"; do
    if [[ "$ENV" == "$s" ]]; then valid=1; break; fi
done

if [[ $valid -eq 0 ]]; then
    echo "WARNING: '$ENV' is not in the known environment list."
    echo "Supported environments:"
    for s in "${SUPPORTED[@]}"; do echo "  $s"; done
    echo "Continuing anyway — PlatformIO will error if the env does not exist."
fi

# ── Flash ─────────────────────────────────────────────────────────────────
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo " Rivr Flash Tool"
echo " Environment : $ENV"
echo " PlatformIO  : $PIO"
echo " Project     : $REPO_DIR"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

cd "$REPO_DIR"
"$PIO" run --environment "$ENV" --target upload

echo ""
echo "Flash complete.  Open the serial monitor with:"
echo "  ./scripts/monitor.sh"
echo "  or: $PIO device monitor --baud 115200"
