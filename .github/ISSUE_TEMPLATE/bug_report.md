---
name: Bug report
about: Something is not working as expected
title: "bug: <short description>"
labels: ["bug", "triage"]
assignees: []
---

## Summary

<!-- One paragraph describing the unexpected behaviour. -->

## Steps to reproduce

1.
2.
3.

## Expected behaviour

<!-- What should happen. -->

## Actual behaviour

<!-- What actually happens. Paste log output if available. -->

## Supportpack output

Run `supportpack` in the serial monitor and paste the full `@SUPPORTPACK` block here.
See [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md#collecting-a-supportpack).

```json
@SUPPORTPACK { ... }
```

## Metrics snapshot

Paste the relevant `@MET` line(s) from the serial monitor:

```
@MET {"rx_fail":0,"rx_dup":0,...}
```

## Environment

| Field | Value |
|---|---|
| Firmware version / commit | |
| PlatformIO env | e.g. `repeater_esp32devkit_e22_900` |
| Board | e.g. ESP32 DevKit V1 |
| Radio module | e.g. EBYTE E22-900M30S |
| Frequency | e.g. 869.480 MHz |
| Number of nodes | |
| Build flags (non-default) | |

## Additional context

<!-- Any other information: scope of failure, when it started, related issues. -->
