#!/usr/bin/env python3
"""
rivr_mesh_map.py — Rivr mesh topology visualizer.

Reads @MET and @NBR JSON lines from one or more node log files (or stdin),
builds a neighbor graph, and renders an ASCII topology map.

Usage:
    # From log files collected with scripts/monitor.sh or pio device monitor:
    python3 tools/rivr-mesh-map/rivr_mesh_map.py node_a.log node_b.log node_c.log

    # From stdin (single node live):
    ./scripts/monitor.sh | python3 tools/rivr-mesh-map/rivr_mesh_map.py --stdin

    # Continuous live refresh (all nodes piped through a multiplexer):
    python3 tools/rivr-mesh-map/rivr_mesh_map.py --live --port /dev/ttyUSB0

Output example:
    ┌──────────────────────────────────────────────────────┐
    │  Rivr Mesh Map  •  3 nodes  •  5 links               │
    ├──────────────────────────────────────────────────────┤
    │  Nodes:                                              │
    │    A509649C  RIVR-1   (client)    rx=121 tx=34       │
    │    DEADBEEF  RIVR-2   (repeater)  rx=88  tx=41       │
    │    CAFEB0BA  RIVR-3   (client)    rx=54  tx=12       │
    ├──────────────────────────────────────────────────────┤
    │  Topology:                                           │
    │    A509649C ──(-78 dBm / snr=6)──▶ DEADBEEF         │
    │    A509649C ──(-91 dBm / snr=2)──▶ CAFEB0BA         │
    │    DEADBEEF ──(-72 dBm / snr=5)──▶ CAFEB0BA         │
    │    DEADBEEF ──(-80 dBm / snr=4)──▶ A509649C         │
    │    CAFEB0BA ──(-89 dBm / snr=1)──▶ DEADBEEF         │
    └──────────────────────────────────────────────────────┘

Requires: Python ≥ 3.8, stdlib only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from datetime import datetime
import time
from typing import Any, Dict, List, Optional, Tuple

# ── Regex patterns ─────────────────────────────────────────────────────────

MET_RE = re.compile(r"@MET\s+(\{.*\})")
NBR_RE = re.compile(r"@NBR\s+(\{.*\})")

# ── Data model ─────────────────────────────────────────────────────────────

class NodeInfo:
    def __init__(self, node_id: int) -> None:
        self.node_id  = node_id
        self.callsign = ""
        self.role     = "unknown"
        self.rx_total = 0
        self.tx_total = 0
        self.dc_pct   = 0
        self.uptime   = 0
        self.neighbors: Dict[int, "LinkInfo"] = {}

    def id_str(self) -> str:
        return f"{self.node_id:08X}"

    def label(self) -> str:
        cs = f"  {self.callsign}" if self.callsign else ""
        return f"{self.id_str()}{cs}"


class LinkInfo:
    def __init__(self, peer_id: int, rssi: int, snr: int, score: float) -> None:
        self.peer_id = peer_id
        self.rssi    = rssi
        self.snr     = snr
        self.score   = score

    def link_str(self) -> str:
        return f"rssi={self.rssi:4d} dBm  snr={self.snr:3d}  score={self.score:.2f}"


# ── Parser ────────────────────────────────────────────────────────────────

class Graph:
    def __init__(self) -> None:
        self.nodes: Dict[int, NodeInfo] = {}

    def _node(self, node_id: int) -> NodeInfo:
        if node_id not in self.nodes:
            self.nodes[node_id] = NodeInfo(node_id)
        return self.nodes[node_id]

    def ingest_met(self, payload: Dict[str, Any]) -> None:
        raw_id = payload.get("node_id")
        if raw_id is None:
            return
        node_id = int(raw_id)
        n = self._node(node_id)
        n.callsign = str(payload.get("callsign", ""))
        n.role     = ("repeater" if payload.get("fabric_repeater")
                      else "client" if payload.get("role_client")
                      else n.role)
        n.rx_total = int(payload.get("rx_total", n.rx_total))
        n.tx_total = int(payload.get("tx_total", n.tx_total))
        n.dc_pct   = int(payload.get("dc_pct",   n.dc_pct))
        n.uptime   = int(payload.get("uptime_ms", n.uptime))

        # Embedded neighbor table (nested in @MET as "nbr" array)
        nbr_list = payload.get("nbr") or payload.get("neighbors_detail") or []
        for entry in nbr_list:
            if not isinstance(entry, dict):
                continue
            peer = entry.get("peer") or entry.get("peer_id")
            if peer is None:
                continue
            peer_id = int(peer)
            n.neighbors[peer_id] = LinkInfo(
                peer_id = peer_id,
                rssi    = int(entry.get("rssi", 0)),
                snr     = int(entry.get("snr", 0)),
                score   = float(entry.get("score", 0.0)),
            )

    def ingest_nbr(self, source_id: int, payload: Dict[str, Any]) -> None:
        n = self._node(source_id)
        peer_list = payload.get("peers") or payload.get("entries") or []
        for entry in peer_list:
            if not isinstance(entry, dict):
                continue
            peer = entry.get("peer") or entry.get("id") or entry.get("peer_id")
            if peer is None:
                continue
            peer_id = int(str(peer), 16) if isinstance(peer, str) else int(peer)
            n.neighbors[peer_id] = LinkInfo(
                peer_id = peer_id,
                rssi    = int(entry.get("rssi", 0)),
                snr     = int(entry.get("snr",  0)),
                score   = float(entry.get("score", 0.0)),
            )

    def ingest_line(self, line: str, source_id: Optional[int] = None) -> None:
        """Parse one @MET or @NBR line and update the graph."""
        line = line.strip()
        m = MET_RE.search(line)
        if m:
            try:
                self.ingest_met(json.loads(m.group(1)))
            except (json.JSONDecodeError, ValueError):
                pass
            return

        m = NBR_RE.search(line)
        if m:
            try:
                data = json.loads(m.group(1))
                # Try to find node_id in payload; fall back to source_id hint
                nid = data.get("node_id") or data.get("source")
                if nid is not None:
                    source_id = int(nid)
                if source_id is not None:
                    self.ingest_nbr(source_id, data)
            except (json.JSONDecodeError, ValueError):
                pass

    def ingest_file(self, path: str) -> None:
        """Read all lines from a log file."""
        with open(path, encoding="utf-8", errors="replace") as f:
            last_node_id: Optional[int] = None
            for line in f:
                # Track last-seen node_id so @NBR lines can be attributed
                m = MET_RE.search(line)
                if m:
                    try:
                        d = json.loads(m.group(1))
                        nid = d.get("node_id")
                        if nid is not None:
                            last_node_id = int(nid)
                    except (json.JSONDecodeError, ValueError):
                        pass
                self.ingest_line(line, source_id=last_node_id)

    def all_links(self) -> List[Tuple[int, int, "LinkInfo"]]:
        """Return all directed edges as (src_id, dst_id, LinkInfo)."""
        links = []
        for src_id, node in self.nodes.items():
            for dst_id, link in node.neighbors.items():
                links.append((src_id, dst_id, link))
        return sorted(links, key=lambda t: (t[2].rssi * -1))


# ── Renderer ──────────────────────────────────────────────────────────────

def _ansi(code: str) -> str:
    return f"\033[{code}" if sys.stdout.isatty() else ""

BOLD  = _ansi("1m")
DIM   = _ansi("2m")
RESET = _ansi("0m")
CYAN  = _ansi("36m")
GREEN = _ansi("32m")
YELLOW = _ansi("33m")
RED   = _ansi("31m")


def _rssi_colour(rssi: int) -> str:
    if rssi > -70:  return GREEN
    if rssi > -85:  return YELLOW
    return RED


def render(graph: Graph) -> None:
    nodes = graph.nodes
    links = graph.all_links()
    w = 62

    print(f"┌{'─' * w}┐")
    ts = datetime.now().strftime("%H:%M:%S")
    title = f"  {CYAN}{BOLD}Rivr Mesh Map{RESET}  •  {len(nodes)} nodes"
    print(f"│{title}  •  {len(links)} links  •  {ts}"
          f"{' ' * max(0, w - 36 - len(str(len(nodes))) - len(str(len(links))))}│")

    # ── Node list ──────────────────────────────────────────────────────────
    print(f"├{'─' * w}┤")
    print(f"│  {BOLD}Nodes:{RESET}{' ' * (w - 7)}│")
    for node in sorted(nodes.values(), key=lambda n: n.node_id):
        cs = f"  {node.callsign:<10}" if node.callsign else " " * 12
        role = f"({node.role:<8})" if node.role != "unknown" else "          "
        line = (f"    {CYAN}{node.id_str()}{RESET}{cs}  {DIM}{role}{RESET}"
                f"  rx={node.rx_total:<5} tx={node.tx_total}")
        pad = " " * max(0, w - len(re.sub(r"\033\[[^m]*m", "", line)) + 2)
        print(f"│{line}{pad}│")

    # ── Topology links ─────────────────────────────────────────────────────
    if links:
        print(f"├{'─' * w}┤")
        print(f"│  {BOLD}Topology:{RESET}{' ' * (w - 10)}│")
        for src_id, dst_id, lnk in links:
            src_cs = nodes.get(src_id)
            dst_cs = nodes.get(dst_id)
            src_label = src_cs.callsign if src_cs and src_cs.callsign else f"{src_id:08X}"
            dst_label = dst_cs.callsign if dst_cs and dst_cs.callsign else f"{dst_id:08X}"
            col = _rssi_colour(lnk.rssi)
            score_str = f"score={lnk.score:.2f}"
            link_str  = f"({col}{lnk.rssi:4d} dBm{RESET} / snr={lnk.snr:3d}  {score_str})"
            line = f"    {BOLD}{src_label:>12}{RESET} ──{link_str}──▶ {BOLD}{dst_label}{RESET}"
            pad  = " " * max(0, w - len(re.sub(r"\033\[[^m]*m", "", line)) + 2)
            print(f"│{line}{pad}│")
    else:
        print(f"├{'─' * w}┤")
        print(f"│  {DIM}No neighbor data yet — wait for @MET lines with neighbor tables.{RESET}"
              f"{' ' * 0}│")

    print(f"└{'─' * w}┘", flush=True)


# ── Live mode (single node via pyserial) ──────────────────────────────────

def live_mode(port: str, baud: int) -> None:
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: 'pyserial' is required for --live mode.")
        print("Install it with:  pip install pyserial")
        sys.exit(1)

    graph = Graph()
    print(f"Connecting to {port} at {baud} baud …", flush=True)
    last_render = 0.0
    try:
        with serial.Serial(port, baud, timeout=1.0) as ser:
            print("Connected.  Watching for @MET / @NBR lines …\n", flush=True)
            while True:
                line = ser.readline().decode("utf-8", errors="replace")
                if line.strip():
                    graph.ingest_line(line)
                    now = time.monotonic()
                    if now - last_render > 2.0 and graph.nodes:
                        if sys.stdout.isatty():
                            # Count rendered lines so we can overwrite them
                            n_lines = 5 + len(graph.nodes) + len(graph.all_links())
                            print(_ansi(f"{n_lines}A") + _ansi("0J"), end="")
                        render(graph)
                        last_render = now
    except KeyboardInterrupt:
        print("\nDisconnected.")


# ── Entry point ───────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Rivr mesh topology visualizer — builds a neighbor graph from @MET logs.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("files", nargs="*", metavar="LOG",
                    help="Log files to process (one per node, or mixed)")
    ap.add_argument("--stdin", action="store_true",
                    help="Read from stdin instead of files")
    ap.add_argument("--live", action="store_true",
                    help="Connect to a serial port and refresh the map continuously")
    ap.add_argument("--port", metavar="PORT",
                    help="Serial port for --live mode (e.g. /dev/ttyUSB0)")
    ap.add_argument("--baud", metavar="BAUD", type=int, default=115200,
                    help="Baud rate for --live mode (default: 115200)")
    args = ap.parse_args()

    if args.live:
        if not args.port:
            ap.error("--live requires --port")
        live_mode(args.port, args.baud)
        return

    graph = Graph()

    if args.stdin or not args.files:
        if not args.stdin and not args.files:
            ap.error("Provide log files as arguments or use --stdin")
        try:
            last_node_id: Optional[int] = None
            for line in sys.stdin:
                graph.ingest_line(line)
        except KeyboardInterrupt:
            pass
    else:
        for path in args.files:
            graph.ingest_file(path)

    if not graph.nodes:
        print("No node data found.  Check that your log files contain @MET lines.",
              file=sys.stderr)
        sys.exit(1)

    render(graph)


if __name__ == "__main__":
    main()
