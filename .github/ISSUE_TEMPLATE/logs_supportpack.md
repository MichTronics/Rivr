---
name: Logs / supportpack submission
about: Provide diagnostic logs to help investigate an existing issue
title: "logs: <issue number or description>"
labels: ["logs", "diagnostics"]
assignees: []
---

## Related issue

Closes / relates to #

## How to collect

```bash
# 1. Open the serial monitor
~/.platformio/penv/bin/pio device monitor -e repeater_esp32devkit_e22_900

# 2. Type the supportpack command and copy the output
> supportpack
```

## Supportpack output

<!-- Paste the full @SUPPORTPACK block. -->

```json
@SUPPORTPACK { ... }
```

## Serial log (last ~100 lines)

<!-- Paste the serial monitor output surrounding the failure. -->

```
[I][MAIN] ...
[W][RADIO] ...
@MET {...}
```

## Replay trace (if available)

If you ran the node with `RIVR_TRACE_ENABLE=1` or captured a `.jsonl` trace
via `rivr_host --capture`, attach the file here.

## Environment

| Field | Value |
|---|---|
| Firmware version / commit | |
| PlatformIO env | |
| Uptime at capture | |
| Approximate traffic level | e.g. ~3 msg/min, 5 nodes |
