# rivr-monitor — Live Rivr Node Monitor

Connects to a Rivr node's serial port and displays live mesh statistics,
parsing `@MET` JSON lines output by the firmware.

## Requirements

- Python ≥ 3.8
- No external packages required for `--stdin` mode
- `pip install pyserial` for direct serial port mode (`--port`)

## Usage

```bash
# Direct serial connection (requires pyserial)
python3 tools/rivr-monitor/rivr_monitor.py --port /dev/ttyUSB0

# Pipe from the PlatformIO monitor
./scripts/monitor.sh | python3 tools/rivr-monitor/rivr_monitor.py --stdin

# Process a saved log file
cat node_log.txt | python3 tools/rivr-monitor/rivr_monitor.py --stdin

# Quiet mode — only show the stats box, suppress other serial output
python3 tools/rivr-monitor/rivr_monitor.py --port /dev/ttyUSB0 --quiet
```

## Output

```
┌──────────────────────────────────────────────────────────────┐
│  Rivr Monitor  •  Node 0xA509649C  •  up 3m 12s  •  14:32:05 │
├──────────────────────────────────────────────────────────────┤
│  Neighbors          3         Routes          2         TX queue   0       │
│  RX frames          121       TX frames       34        Duty  ████░░░░░░░░░░   6%  │
│  CRC fail           0         Dedupe drop     12        Fabric drop 0       │
│  Fwd attempt        87        Retries         2         Route hits  54      │
└──────────────────────────────────────────────────────────────┘
```

## Firmware requirements

The monitor parses `@MET` JSON lines.  These are emitted periodically by the
firmware when `RIVR_FEATURE_METRICS = 1` (enabled by default).

To force an immediate metrics dump, use the CLI:

```
rivr> metrics
```
