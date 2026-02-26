# RIVR — Bouwhandleiding

## Vereisten

| Gereedschap | Minimale versie | Installatie |
|---|---|---|
| Rust | 1.75+ | `rustup update stable` |
| ESP-IDF | 5.1+ | https://docs.espressif.com/projects/esp-idf |
| espup | recentste | `cargo install espup` (voor Xtensa cross-compilatie) |
| PlatformIO | 6.x *(optioneel)* | `pip install platformio` |

---

## Stap 1 — Bouw de Rust statische bibliotheek

De Rust-bibliotheek **moet** gebouwd worden vóór de C-firmware.

### Debug (ontwikkeling)

```powershell
cd e:\Projects\Rivr\rivr_core
cargo build --features ffi
# Uitvoer:  target\debug\librivr_core.a
```

### Release (voor flashen)

```powershell
cargo build --features ffi --release
# Uitvoer:  target\release\librivr_core.a
```

### Cross-compilatie voor ESP32 (Xtensa LX6)

```powershell
# Installeer de Xtensa Rust-toolchain (eenmalig):
espup install

# Cross-compileer:
cargo +esp build --target xtensa-esp32-espidf --features ffi --release
# Uitvoer:  target\xtensa-esp32-espidf\release\librivr_core.a
```

> `main/CMakeLists.txt` vindt de bibliotheek automatisch — eerst in
> `target\release\`, daarna in `target\debug\`. Geen handmatig kopiëren nodig.

### Desktop-demo's (geen hardware)

```powershell
cd e:\Projects\Rivr\rivr_core
cargo run -p rivr_host
```

Voert 8 demo's uit voor elke operator-categorie plus Replay 2.0.

### `rivrc` — RIVR-broncompiler CLI

```powershell
# Druk knoop-grafiek af
cargo run -p rivr_host --bin rivrc -- mijn_programma.rivr

# CI-modus (exit 0 = ok, 1 = fout, geen uitvoer)
cargo run -p rivr_host --bin rivrc -- --check mijn_programma.rivr

# Of na installeren:
cargo install --path rivr_host --bin rivrc
rivrc mijn_programma.rivr
```

---

## Stap 2a — Bouwen via PlatformIO *(aanbevolen)*

### Bouwervaringen

| Omgeving | Rol | Belangrijkste vlaggen |
|---|---|---|
| `esp32_sim` | Simulatie — geen SX1262 vereist | `RIVR_SIM_MODE=1`, `RIVR_SIM_TX_PRINT=1` |
| `esp32_hw` | Standaard hardwareknoop | `FEATURE_DISPLAY=1` |
| `repeater_esp32devkit_e22_900` | Dedicated relay met Rivr Fabric | `RIVR_FABRIC_REPEATER=1`, `RIVR_BUILD_REPEATER=1`, `FEATURE_DISPLAY=1` |
| `client_esp32devkit_e22_900` | Chat-/data-ontvanger; geen relay | `RIVR_ROLE_CLIENT=1`, `FEATURE_DISPLAY=1` |

De repeater- en cliënt-omgevingen bevatten via `-include` een boardspecifieke variantheader:  
`variants/esp32devkit_e22_900_repeater/config.h` respectievelijk  
`variants/esp32devkit_e22_900_client/config.h`.

### Seriële CLI van de cliëntknoop

De cliëntfirmware (`client_esp32devkit_e22_900`) activeert een interactieve
chatshell over UART0 op 115200 baud.  Open de seriële monitor na het flashen:

```bash
~/.platformio/penv/bin/pio run -e client_esp32devkit_e22_900 -t upload
~/.platformio/penv/bin/pio device monitor -e client_esp32devkit_e22_900
```

**Opstartbanner:**
```
Rivr Client Node Ready
Type 'help' for commands
Node ID : 0xdeadbeef  Net ID : 0x0000
> 
```

**Beschikbare commando’s:**

| Commando | Effect |
|---|---|
| `chat <bericht>` | Codeer en verzend een `PKT_CHAT`-frame via LoRa |
| `id` | Druk het 32-bits node-ID en net-ID af |
| `help` | Toon beschikbare commando’s |

**Inkomende berichten** van andere meshnodes worden automatisch afgedrukt:

```
[CHAT][cafebabe]: hallo van de repeater
> 
```

De `> `-prompt wordt opnieuw afgedrukt na elk inkomend bericht, zodat
in-uitvoering-zijnd invoer zichtbaar blijft.  Maximale berichtlengte: 128
tekens (regelsbuffer); berichten worden gecodeerd via het standaard binaire
`PKT_CHAT`-formaat.

### Simulatiemodus (geen SX1262-hardware vereist)

```powershell
cd e:\Projects\Rivr

# Bouw simulatiefirmware
pio run -e esp32_sim

# Flash + open seriële monitor
pio run  -e esp32_sim -t upload
pio device monitor -e esp32_sim --baud 115200
```

**Verwachte UART-uitvoer:**

```
I (...) MAIN: ═══ RIVR Embedded Node booting ═══
I (...) MAIN: *** SIMULATION MODE: no real SX1262 hardware ***
I (...) RADIO: radio_init_buffers_only: ringbufs ready (SIM MODE)
I (...) RIVR_SINK: sinks registered: rf_tx, usb_print, log
I (...) RIVR_EMBED: engine ready
I (...) MAIN: [SIM] injecting 5 PKT_DATA + 5 PKT_CHAT binary frames
I (...) MAIN: [SIM] frames injected, entering main loop
[RIVR-TX] <bytes len=...>              ← usb_print: PKT_CHAT passeert filter
I (...) RIVR_SINK: rf_tx: queued ...   ← rf_tx: 5 CHAT-frames in wachtrij
I (...) MAIN: [SIM] TX type=CHAT ...   ← tx_drain_loop uitvoer (×5)
```

### Hardwaremodus (SX1262 aangesloten)

```powershell
cd e:\Projects\Rivr\rivr_core
cargo build --features ffi --release
cd ..

pio run -e esp32_hw -t upload
pio device monitor --baud 115200
```

---

## Stap 2b — Bouwen via ESP-IDF (idf.py)

```powershell
# Activeer ESP-IDF-omgeving (eenmalig per shell):
. $env:IDF_PATH\export.ps1       # PowerShell
# source $IDF_PATH/export.sh     # Linux / macOS

cd e:\Projects\Rivr

# Stel doel in (eenmalig per checkout):
idf.py set-target esp32

# Simulatiebouw:
idf.py -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1 build

# Hardwarebouw (geen extra vlaggen):
idf.py build

# Flash + monitor:
idf.py -p COM3 flash monitor
```

---

## Stap 3 — Verifieer RIVR-filtering

De simulatie injecteert 5 PKT_DATA + 5 PKT_CHAT frames.
`filter.pkt_type(1)` laat alleen PKT_CHAT (type byte = 1) door.

| Frame-type | Aantal | Verwacht resultaat |
|---|---|---|
| PKT_DATA (type 6) | 5 | **Weggegooid** — geen uitvoer |
| PKT_CHAT (type 1) | 5 | **Doorgelaten** → `[RIVR-TX]` + rf_tx wachtrij |

Als er vijf `[RIVR-TX]`-regels verschijnen en er geen DATA-regels tussen zitten,
werkt de volledige pijplijn (FFI, filter, budget, sink-dispatch) correct.

---

## VS Code-extensie

De extensie in `tools/vscode-rivr/` voegt syntaxisaccentuering en snippets
toe voor `.rivr`-bestanden.

### Installeren (ontwikkeling)

1. Open `tools/vscode-rivr/` in VS Code.
2. Voer **Extensions: Install from VSIX…** uit of druk op **F5** om
   een Extension Development Host te starten.
3. Open een `.rivr`-bestand voor syntaxisaccentuering.

### Snippets

| Prefix | Invoegt |
|---|---|
| `src-rf` | RF-bron met PKT_CHAT-relay |
| `src-timer` | Periodieke timer-bron |
| `prog-beacon` | Volledig baken + chat-relay-programma |
| `prog-mesh` | Volledig mesh-programma (chat + data-relay) |
| `emit` | Emit-blok |
| `let` | Let-binding met pijplijn |

---

---

## SX1262-pinbezetting

Standaardbedrading — geldt voor alle omgevingen.  Voor de repeater- en
cliënt-varianten staan de pinnen in de variantheader
(`variants/esp32devkit_e22_900_repeater/config.h` of
`variants/esp32devkit_e22_900_client/config.h`), omsloten door `#ifndef`,
zodat je elk pin kunt overschrijven met `-DPIN_SX1262_SCK=<gpio>` in
`platformio.ini` of op de commandoregel.  Voor `esp32_hw` en `esp32_sim`
staan de pins in `firmware_core/platform_esp32.h`.

| Signaal | ESP32 GPIO | Opmerking |
|---|---|---|
| SCK | 18 | VSPI_CLK |
| MOSI | 23 | VSPI_MOSI |
| MISO | 19 | VSPI_MISO |
| NSS (CS) | 5 | Actief laag |
| BUSY | 32 | SX1262 gereed-vlag |
| RESET | 25 | Actief laag |
| DIO1 (IRQ) | 33 | TxDone / RxDone / Timeout |
| RXEN | 14 | Antenneschakelaar — HOOG = ontvangen |
| TXEN | 13 | Antenneschakelaar — HOOG = zenden (ook via SX1262 DIO2) |

Pas de pin-defines aan in de variantheader of geef `-DPIN_SX1262_NSS=<gpio>`
mee als bouwvlag — het `#ifndef`-bewaker past de overschrijving toe.

---

## Variantheaders

Variantheaders staan onder `variants/<board>/config.h` en worden door
PlatformIO via `-include` als bouwvlag geïnjecteerd.  Elke macro is
omsloten door `#ifndef`, zodat een `-D`-overschrijving altijd wint.

Maak een nieuw variantheader om naar een ander board te porten:

```c
// variants/mijn_board/config.h
#ifndef RIVR_RF_FREQ_HZ
#  define RIVR_RF_FREQ_HZ  915000000UL   // AU915
#endif
#ifndef PIN_SX1262_SCK
#  define PIN_SX1262_SCK   18
#endif
// … overige pins …
#ifndef RIVR_FABRIC_REPEATER
#  define RIVR_FABRIC_REPEATER 0
#endif
```

Voeg daarna een omgeving toe aan `platformio.ini`:

```ini
[env:mijn_board]
extends = base_hw
build_flags =
    ${base_hw.build_flags}
    -include variants/mijn_board/config.h
```

---

## Rivr Fabric instelbare macro's

Deze macro's sturen het congestiebeleid wanneer `RIVR_FABRIC_REPEATER=1`.
Stel ze in via de variantheader of als `-D`-bouwvlag.

| Macro | Standaard | Beschrijving |
|---|---|---|
| `RIVR_FABRIC_DROP_THRESHOLD` | `80` | Score ≥ waarde → **DROP** (geen relay) |
| `RIVR_FABRIC_DELAY_THRESHOLD` | `50` | Score ≥ waarde → **DELAY** met maximaal `MAX_EXTRA_DELAY_MS` |
| `RIVR_FABRIC_LIGHT_DELAY_THRESHOLD` | `20` | Score ≥ waarde → korte jitter-vertraging (lage congestie) |
| `RIVR_FABRIC_MAX_EXTRA_DELAY_MS` | `1000` | Maximale extra vertraging in milliseconden |
| `RIVR_FABRIC_BLACKOUT_GUARD_SCORE` | `95` | Vanaf deze score wordt DROP vervangen door DELAY (anti-blackout) |

De score wordt berekend over een schuivend venster van 60 seconden:
```
score = clamp(rx_per_s×2 + dc_blocked_per_s×25 + tx_fail_per_s×10, 0, 100)
```

Alleen `PKT_CHAT`- en `PKT_DATA`-relay wordt geblokkeerd door Fabric.  Alle
andere pakkettypen (`PKT_BEACON`, `ROUTE_REQ`, `ROUTE_RPL`, `PKT_ACK`,
`PKT_PROG_PUSH`) worden altijd doorgelaten.

---

## Overschakelen van simulatie naar hardware

1. Bedraat de SX1262 volgens de pintabel hierboven.
2. Verwijder `-DRIVR_SIM_MODE=1` en `-DRIVR_SIM_TX_PRINT=1` (of gebruik `esp32_hw`).
3. Bekijk de `// TODO(SX1262):`-opmerkingen in:
   - `firmware_core/main.c` — vervang `radio_sim_init()` door `radio_init()`
   - `firmware_core/radio_sx1262.c` — verwijder `radio_init_buffers_only()`-bewaker
4. Flash: `pio run -e esp32_hw -t upload`

---

## Probleemoplossing

| Symptoom | Oorzaak / Oplossing |
|---|---|
| `FATAL_ERROR: librivr_core.a not found` | Voer eerst `cargo build --features ffi` uit |
| `rivr_engine_init failed: code 1` | Parsefout (`RIVR_ERR_PARSE`) — controleer `RIVR_ACTIVE_PROGRAM` of NVS-programma |
| `rivr_engine_init failed: code 2` | Compilatiefout (`RIVR_ERR_COMPILE`) — controleer programma-semantiek (onbekende bron, enz.) |
| Geen `[RIVR-TX]`-regels | `rivr_set_emit_dispatch` niet aangeroepen, of `filter.pkt_type`-mismatch |
| `rf_tx: queue full`-waarschuwingen | Verlaag de framesnelheid of vergroot `RF_TX_QUEUE_CAP` in `radio_sx1262.h` |
| ESP32-crash bij opstarten in sim-modus | `platform_init()` werd aangeroepen — correct voor hardware-builds |
| CRC-fouten in `protocol_decode` | Frame beschadigd in ring-buffer-kopie; controleer `frame.len`-grenzen |
| NVS-programma laadt niet | Voer `nvs_flash_erase()` eenmalig uit; controleer retourwaarde van `nvs_open("rivr", ...)` |
