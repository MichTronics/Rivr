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

```bash
cd rivr_core
cargo build --features ffi
# Uitvoer:  target/debug/librivr_core.a
```

### Release (voor flashen)

```bash
cargo build --features ffi --release
# Uitvoer:  target/release/librivr_core.a
```

### Cross-compilatie voor ESP32 (Xtensa LX6)

```bash
# Installeer de Xtensa Rust-toolchain (eenmalig):
espup install

# Cross-compileer:
cargo +esp build --target xtensa-esp32-espidf --features ffi --release
# Uitvoer:  target/xtensa-esp32-espidf/release/librivr_core.a
```

> `main/CMakeLists.txt` vindt de bibliotheek automatisch — eerst in
> `target/release/`, daarna in `target/debug/`. Geen handmatig kopiëren nodig.

### Desktop-demo's (geen hardware)

```bash
cargo run -p rivr_host
```

Voert 8 demo's uit voor elke operator-categorie plus Replay 2.0.

### `rivrc` — RIVR-broncompiler CLI

```bash
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
| `repeater_<board>` | Dedicated relay met Rivr Fabric | `RIVR_FABRIC_REPEATER=1`, `FEATURE_DISPLAY=1` |
| `client_<board>` | Chat-/data-ontvanger; geen relay | `RIVR_ROLE_CLIENT=1`, `FEATURE_DISPLAY=1` |
| `client_<board>_ble` | Client + BLE-brug | `RIVR_ROLE_CLIENT=1`, `RIVR_FEATURE_BLE=1`, `sdkconfig.ble` |

Ondersteunde boards: `esp32devkit_e22_900`, `lilygo_lora32_v21`, `heltec_lora32_v2`, `heltec_lora32_v3`, `lilygo_t3s3`, `lilygo_tbeam_sx1262`.

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
╔══════════════════════════════════╗
║   Rivr Client Node — Serial CLI  ║
╚══════════════════════════════════╝
Node ID  : 0xDEADBEEF
Callsign : CALL
Net ID   : 0x0000
Type 'help' for full command list.
> 
```

**Beschikbare commando’s:**

| Commando | Effect |
|---|---|
| `chat <bericht>` | Codeer en verzend een `PKT_CHAT`-frame via LoRa |
| `id` | Druk het 32-bits node-ID, roepnaam en net-ID af |
| `info` | Druk bouwinfo af (omgeving, git-SHA, radioprofiel) |
| `metrics` | Druk alle tellers als JSON af (`@MET`-regel) |
| `policy` | Druk `@POLICY` JSON af (huidige parameters + statistieken) |
| `supportpack` | JSON-dump: bouwinfo + volledige statistieken-snapshot (`@SUPPORTPACK`) |
| `neighbors` | Toon live buurttabel met RSSI/SNR/linkscores |
| `routes` | Toon route-cache met scores en leeftijden |
| `set callsign <CS>` | Stel roepnaam in en sla op (1–11 tekens: A–Z a–z 0–9 -) |
| `set netid <HEX>` | Stel net-ID in en sla op (hex 0…FFFF) |
| `log <debug\|metrics\|silent>` | Stel logverbositeit in |
| `help` | Toon deze commandolijst |

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

```bash
# Bouw simulatiefirmware
pio run -e esp32_sim

# Flash + open seriële monitor
pio run -e esp32_sim -t upload
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

```bash
cd rivr_core && cargo build --features ffi --release && cd ..
pio run -e esp32_hw -t upload
pio device monitor --baud 115200
```

---

## Stap 2b — Bouwen via ESP-IDF (idf.py)

```bash
# Activeer ESP-IDF-omgeving (eenmalig per shell):
source $IDF_PATH/export.sh

# Stel doel in (eenmalig per checkout):
idf.py set-target esp32

# Simulatiebouw:
idf.py -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1 build

# Hardwarebouw (geen extra vlaggen):
idf.py build

# Flash + monitor (vervang /dev/ttyUSB0 door jouw poort):
idf.py -p /dev/ttyUSB0 flash monitor
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
## OLED-schermbedrading (SSD1306 128×64)

Alle hardwareomgevingen schakelen het SSD1306-stuurprogramma in (`FEATURE_DISPLAY=1`).
Het stuurprogramma draait als een taak met lage prioriteit op CPU1 en beheert
alle I²C-communicatie zonder de hoofdtaak te blokkeren.

### Standaard I²C-pinnen

| Signaal | ESP32 GPIO | Opmerking |
|---|---|---|
| SDA | 21 | Overschrijven met `-DPIN_DISPLAY_SDA=<gpio>` |
| SCL | 22 | Overschrijven met `-DPIN_DISPLAY_SCL=<gpio>` |
| VCC | 3,3 V | **Niet** op 5 V aansluiten; SSD1306 I²C-lijnen zijn 3,3 V |
| GND | GND | |

Pull-ups (4,7 kΩ naar 3,3 V) zijn vereist op SDA en SCL; de meeste SSD1306-breakout-
borden bevatten ze al.  De ESP-IDF I²C-master-driver schakelt ook zwakke interne
pull-ups in.

### I²C-parameters

| Parameter | Waarde |
|---|---|
| Snelheid | 400 kHz (fast mode) |
| Adresseringsmodus | Horizontaal (Adafruit-compatibel); volledige 1024-byte GDDRAM in één burst van 1025 bytes doorgezonden |
| Adresdetectie | Automatisch; probeert 0x3C (“SA0 laag”) dan 0x3D (“SA0 hoog”) bij opstarten |
| Verversingssnelheid | Max 5 Hz (200 ms bewaking); pagina roteert automatisch elke 3 s |

### Feature-gate

```c
// Inschakelen in platformio.ini build_flags:
-DFEATURE_DISPLAY=1

// Of in een variantheader:
#define FEATURE_DISPLAY 1
```

Zonder `FEATURE_DISPLAY=1` compileren alle displayfuncties naar lege inline
no-ops; er wordt geen I²C-bus gestart.

### Opstartsequentie

1. I²C-bus aangemaakt; 150 ms VCC-stabilisatietijd.
2. Adres auto-gedetecteerd (0x3C eerst geprobeerd).
3. Volledige initsequentie verzonden (display uit → klokdeling → multiplexer →
   laadbron → horizontale modus → segment-remap → COM-scan → contrast → display aan).
4. **Zelftest** (400 ms): `0xA5` zet alle pixels aan — scherm toont effen wit.
   Als het scherm hier zwart blijft, is er een hardware-probleem tussen ESP32 en OLED.
5. Opstartscherm getoond (node-ID, net-ID, callsign).
6. Normale 5 Hz-verversing begint.

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
| OLED toont niets na opstarten | Controleer SDA/SCL-bedrading (standaard GPIO21/22), VCC 3,3 V en 4,7 kΩ pull-ups op SDA+SCL |
| `SSD1306 not found on SDA=GPIO21 SCL=GPIO22` | Chip reageert niet op 0x3C of 0x3D; controleer SA0-pin van module en voeding |
| OLED toont wit bij opstarten, daarna opstartscherm | **Normaal** — 400 ms zelftest (alle pixels aan), daarna opstartscherm |
| OLED bevriest of gaat uit na TX | Voedingsglitch door hoog-vermogen PA; stuurprogramma herstelt automatisch na 3 flush-fouten; voeg bulk-ontkoppelcondensator toe bij OLED VCC |
