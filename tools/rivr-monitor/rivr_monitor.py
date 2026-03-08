#!/usr/bin/env python3
"""
rivr_monitor.py — Live mesh node monitor for Rivr firmware.

Connects to a Rivr node's serial port, parses @MET JSON lines, and
displays live mesh statistics in the terminal.

Usage:
    python3 tools/rivr-monitor/rivr_monitor.py --port /dev/ttyUSB0
    python3 tools/rivr-monitor/rivr_monitor.py --port /dev/ttyUSB0 --baud 115200
    python3 tools/rivr-monitor/rivr_monitor.py --stdin      # pipe from scripts/monitor.sh
    cat rivr_log.txt | python3 tools/rivr-monitor/rivr_monitor.py --stdin

Requires: Python ≥ 3.8, no external packages needed.
Optional: 'pyserial' for direct serial port connections.

Output example:
    ┌─────────────────────────────────────────────────────┐
    │  Rivr Monitor  •  Node 0xA509649C  •  uptime 3 min  │
    ├─────────────────────────────────────────────────────┤
    │  Neighbors  3    Routes  2    TX queue  0           │
    │  RX total   121  TX total 34   Duty cycle  6%       │
    │  CRC fail   0    Dedupe   12   Fabric drop 0        │
    │  Fwd tried  87   Retries  2    Route hits  54       │
    └─────────────────────────────────────────────────────┘
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from datetime import datetime
from typing import Any, Dict, Optional

# ── ANSI escape codes (disabled automatically when not a TTY) ─────────────

def _ansi(code: str) -> str:
    return f"\033[{code}" if sys.stdout.isatty() else ""

CLR_SCREEN  = _ansi("2J\033[H")
CLR_LINE    = _ansi("2K\r")
BOLD        = _ansi("1m")
DIM         = _ansi("2m")
RESET       = _ansi("0m")
GREEN       = _ansi("32m")
YELLOW      = _ansi("33m")
RED         = _ansi("31m")
CYAN        = _ansi("36m")

# ── @MET JSON line parser ─────────────────────────────────────────────────

MET_RE  = re.compile(r"@MET\s+(\{.*\})")
NBR_RE  = re.compile(r"@NBR\s+(\{.*\})")
CHAT_RE = re.compile(r"@CHT\s+(\{.*\})")
SPCK_RE = re.compile(r"@SUPPORTPACK\s+(\{.*\})")

def parse_met_line(line: str) -> Optional[Dict[str, Any]]:
    """Extract the JSON payload from an @MET line.  Returns None on failure."""
    m = MET_RE.search(line)
    if not m:
        return None
    try:
        return json.loads(m.group(1))
    except json.JSONDecodeError:
        return None


def parse_nbr_line(line: str) -> Optional[Dict[str, Any]]:
    m = NBR_RE.search(line)
    if not m:
        return None
    try:
        return json.loads(m.group(1))
    except json.JSONDecodeError:
        return None


# ── Display helpers ───────────────────────────────────────────────────────

def _w(label: str, value: Any, width: int = 8) -> str:
    """Format a label+value cell, right-padding the value to 'width'."""
    return f"  {BOLD}{label:<18}{RESET}{str(value):<{width}}"


def _bar(used_pct: int, width: int = 20) -> str:
    """ASCII duty-cycle bar."""
    filled = max(0, min(width, int(used_pct * width / 100)))
    colour = GREEN if used_pct < 50 else (YELLOW if used_pct < 80 else RED)
    return f"{colour}{'█' * filled}{'░' * (width - filled)}{RESET} {used_pct:3d}%"


def _fmt_uptime(ms: int) -> str:
    s  = ms // 1000
    m  = s  // 60
    h  = m  // 60
    if h > 0:
        return f"{h}h {m % 60}m"
    if m > 0:
        return f"{m}m {s % 60}s"
    return f"{s}s"


# ── Renderer ─────────────────────────────────────────────────────────────

class Monitor:
    def __init__(self, quiet: bool = False) -> None:
        self.quiet     = quiet
        self.latest    = {}           # most recent @MET payload
        self.met_count = 0
        self.last_seen = None

    def ingest(self, line: str) -> None:
        """Process one line from the serial stream."""
        line = line.rstrip("\r\n")

        met = parse_met_line(line)
        if met:
            self.latest    = met
            self.met_count += 1
            self.last_seen = datetime.now()
            if not self.quiet:
                self._render()
            return

        # Pass non-@MET lines through (chat, warn, info) unless quiet
        if not self.quiet:
            if line.startswith("@"):
                # Structured line — print with dim colour
                print(f"{DIM}{line}{RESET}", flush=True)
            else:
                print(line, flush=True)

    def _render(self) -> None:
        d = self.latest
        if not d:
            return

        node_id  = d.get("node_id", "?")
        if isinstance(node_id, int):
            node_id = f"0x{node_id:08X}"

        uptime   = d.get("uptime_ms", 0)
        dc_pct   = d.get("dc_pct", 0)
        nbrs     = d.get("neighbors", d.get("nbr_count", "?"))
        routes   = d.get("routes",    d.get("route_count", "?"))
        rx       = d.get("rx_total",  "?")
        tx       = d.get("tx_total",  "?")
        q_depth  = d.get("q_depth",   "?")

        # Counters that may not be present in older firmware
        crc_fail = d.get("radio_rx_crc_fail",  d.get("crc_fail",    0))
        dedupe   = d.get("rx_dedupe_drop",      d.get("dedupe_drop", 0))
        fwd_att  = d.get("flood_fwd_attempted_total", d.get("fwd_attempt", 0))
        fab_drop = d.get("fabric_drop",          0)
        retries  = d.get("retry_attempt_total",  d.get("retries",    0))
        rc_hit   = d.get("route_cache_hit_total", d.get("rc_hit",    0))
        rcache   = d.get("route_cache",           routes)

        w = 62  # box inner width
        ts = self.last_seen.strftime("%H:%M:%S") if self.last_seen else "?"

        if sys.stdout.isatty():
            # Move cursor up to overwrite previous rendering (5 lines)
            print(_ansi("5A") + _ansi("0J"), end="")

        print(f"┌{'─' * w}┐")
        print(f"│{CYAN}{BOLD}  Rivr Monitor{RESET}  •  Node {CYAN}{node_id}{RESET}"
              f"  •  up {_fmt_uptime(uptime)}"
              f"  •  {ts}"
              f"{' ' * max(0, w - 36 - len(_fmt_uptime(uptime)))}│")
        print(f"├{'─' * w}┤")
        print(f"│{_w('Neighbors', nbrs)}{_w('Routes', rcache)}"
              f"{_w('TX queue', q_depth)}{'':>6}│")
        print(f"│{_w('RX frames', rx)}{_w('TX frames', tx)}"
              f"  Duty  {_bar(dc_pct, 14)}  │")
        print(f"│{_w('CRC fail', crc_fail)}{_w('Dedupe drop', dedupe)}"
              f"{_w('Fabric drop', fab_drop)}│")
        print(f"│{_w('Fwd attempt', fwd_att)}{_w('Retries', retries)}"
              f"{_w('Route hits', rc_hit)}  │")
        print(f"└{'─' * w}┘", flush=True)


# ── Serial reader ─────────────────────────────────────────────────────────

def read_serial(port: str, baud: int, monitor: Monitor) -> None:
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: 'pyserial' is required for direct serial connections.")
        print("Install it with:  pip install pyserial")
        print("Or pipe from scripts/monitor.sh:  ./scripts/monitor.sh | "
              "python3 tools/rivr-monitor/rivr_monitor.py --stdin")
        sys.exit(1)

    print(f"Connecting to {port} at {baud} baud …", flush=True)
    try:
        with serial.Serial(port, baud, timeout=1.0) as ser:
            print(f"Connected.  Waiting for @MET lines …\n", flush=True)
            # Pre-print 7 blank lines so the in-place renderer has room to overwrite
            print("\n" * 7, end="")
            while True:
                line = ser.readline().decode("utf-8", errors="replace")
                if line:
                    monitor.ingest(line)
    except serial.SerialException as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nDisconnected.")


def read_stdin(monitor: Monitor) -> None:
    print("Reading from stdin …\n", flush=True)
    print("\n" * 7, end="")
    try:
        for line in sys.stdin:
            monitor.ingest(line)
    except KeyboardInterrupt:
        print("\nDone.")


# ── Entry point ───────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Rivr live mesh monitor — parses @MET JSON lines from firmware.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--port",  metavar="PORT", help="Serial port (e.g. /dev/ttyUSB0)")
    ap.add_argument("--baud",  metavar="BAUD", type=int, default=115200,
                    help="Baud rate (default: 115200)")
    ap.add_argument("--stdin", action="store_true",
                    help="Read lines from stdin instead of a serial port")
    ap.add_argument("--quiet", action="store_true",
                    help="Suppress non-@MET output; only render the stats box")
    args = ap.parse_args()

    monitor = Monitor(quiet=args.quiet)

    if args.stdin:
        read_stdin(monitor)
    elif args.port:
        read_serial(args.port, args.baud, monitor)
    else:
        ap.error("Specify --port /dev/ttyUSB0 or --stdin")


if __name__ == "__main__":
    main()
