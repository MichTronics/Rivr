# vscode-rivr

VS Code extension for the **RIVR** stream-policy language.

## Features

- **Syntax highlighting** — keywords, built-in operators, sinks, timer sources, annotations, comments, strings, numbers
- **Snippets** — quick starters for common RIVR program patterns:
  - `src-rf` — RF source with PKT_CHAT relay
  - `src-timer` — periodic timer source
  - `prog-beacon` — full beacon + chat relay program
  - `prog-mesh` — full mesh program (chat + data relay)
  - `emit` — emit block
  - `let` — let binding with pipeline
- **Auto-closing pairs** — `{ }`, `( )`, `" "`
- **Language configuration** — comment toggling (`// …`, `/* … */`), indentation, word patterns

## Installation (development)

1. Install dependencies:  
   ```
   npm install
   npm run compile
   ```
2. Press **F5** in VS Code to open an Extension Development Host.
3. Open any `.rivr` file to see syntax highlighting.

## Syntax overview

```rivr
// Relay chat packets, bounded by duty cycle
source rf_rx @lmp = rf;
source beacon_tick = timer(30000);

let chat = rf_rx
  |> filter.pkt_type(1)
  |> budget.toa_us(280000, 0.10, 280000)
  |> throttle.ticks(1);

emit { io.lora.tx(chat); }
emit { io.lora.beacon(beacon_tick); }
```

## Using `rivrc`

The companion `rivrc` CLI (built from `rivr_host`) can check `.rivr` files:

```sh
# Check for errors only (CI):
rivrc --check program.rivr

# Print node graph:
rivrc program.rivr
```

## Supported RIVR constructs

| Construct | Example |
|-----------|---------|
| Source declaration | `source NAME = rf;` |
| Clock annotation | `source NAME @lmp = rf;` |
| Timer source | `source tick = timer(30000);` |
| Let binding | `let x = src \|> op;` |
| Pipeline operators | `filter.pkt_type(1)`, `budget.toa_us(…)`, `throttle.ticks(1)` |
| Emit | `emit { io.lora.tx(x); }` |
| Built-in sinks | `io.lora.tx`, `io.lora.beacon`, `io.usb.print`, `io.debug.dump` |
| Comments | `// line`, `/* block */` |
