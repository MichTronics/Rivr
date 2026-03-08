#!/usr/bin/env bash
# scripts/monitor.sh — Open the PlatformIO serial monitor for a Rivr node.
#
# Usage:
#   ./scripts/monitor.sh                              # auto-detect port, 115200 baud
#   ./scripts/monitor.sh /dev/ttyUSB0                 # explicit port
#   ./scripts/monitor.sh /dev/ttyUSB0 921600          # explicit port + baud
#
# With no port argument PlatformIO will detect the connected ESP32 automatically.
# If multiple boards are connected, specify the port explicitly.
#
# Tip: pipe output through the Python monitor tool for live mesh stats:
#   ./scripts/monitor.sh | python3 tools/rivr-monitor/rivr_monitor.py --stdin
#
# Press Ctrl+] or Ctrl+C to exit the serial monitor.

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

PORT="${1:-}"
BAUD="${2:-115200}"

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo " Rivr Serial Monitor"
echo " Port  : ${PORT:-auto-detect}"
echo " Baud  : $BAUD"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [[ -n "$PORT" ]]; then
    "$PIO" device monitor --port "$PORT" --baud "$BAUD"
else
    "$PIO" device monitor --baud "$BAUD"
fi
