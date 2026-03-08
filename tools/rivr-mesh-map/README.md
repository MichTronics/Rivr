# rivr-mesh-map — Rivr Mesh Topology Visualizer

Reads `@MET` and `@NBR` JSON lines from one or more node logs, builds a
neighbor graph, and renders an ASCII mesh topology map.

## Requirements

- Python ≥ 3.8
- No external packages required for offline log processing
- `pip install pyserial` for live `--port` mode

## Usage

```bash
# Process log files collected from multiple nodes
python3 tools/rivr-mesh-map/rivr_mesh_map.py node_a.log node_b.log node_c.log

# Read from stdin (single node live stream)
./scripts/monitor.sh | python3 tools/rivr-mesh-map/rivr_mesh_map.py --stdin

# Live refresh from a serial port (requires pyserial)
python3 tools/rivr-mesh-map/rivr_mesh_map.py --live --port /dev/ttyUSB0
```

## Output

```
┌──────────────────────────────────────────────────────────────┐
│  Rivr Mesh Map  •  3 nodes  •  5 links  •  14:32:05          │
├──────────────────────────────────────────────────────────────┤
│  Nodes:                                                      │
│    A509649C  RIVR-1       (client  )  rx=121   tx=34         │
│    DEADBEEF  RIVR-2       (repeater)  rx=88    tx=41         │
│    CAFEB0BA  RIVR-3       (client  )  rx=54    tx=12         │
├──────────────────────────────────────────────────────────────┤
│  Topology:                                                   │
│    A509649C ──( -78 dBm / snr=  6  score=0.82)──▶ DEADBEEF  │
│    A509649C ──( -91 dBm / snr=  2  score=0.54)──▶ CAFEB0BA  │
│    DEADBEEF ──( -72 dBm / snr=  5  score=0.88)──▶ CAFEB0BA  │
│    DEADBEEF ──( -80 dBm / snr=  4  score=0.71)──▶ A509649C  │
│    CAFEB0BA ──( -89 dBm / snr=  1  score=0.41)──▶ DEADBEEF  │
└──────────────────────────────────────────────────────────────┘
```

RSSI colours: **green** > −70 dBm · **yellow** −70 to −85 dBm · **red** < −85 dBm

## Collecting logs

On each node, run:

```
rivr> supportpack
```

Or capture continuous serial output:

```bash
./scripts/monitor.sh > node_a.log
# Ctrl+C to stop
```

The tool understands any log file that contains `@MET` and/or `@NBR` lines.
