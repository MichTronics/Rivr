# RIVR — Taalreferentie

RIVR is een kleine stream-verwerkingstaal.  Een RIVR-programma bestaat uit
**bronverklaringen**, **`let`-stream-bindingen** en een **`emit`-blok**.

---

## Grammatica (informeel)

```
programma   = instructie*
instructie  = bronverklaring | let-instr | emit-instr
bronverklaring = "source" ID ["@" ID] "=" bron-soort ";"
let-instr   = "let" ID "=" uitdr ";"
emit-instr  = "emit" "{" (sink ";")+ "}"
uitdr       = primair ("|>" pijp-op)*
primair     = ID | TEKST | GETAL | "merge" "(" ID "," ID ")"
sink        = "io.usb.print"    "(" ID ")"
            | "io.lora.tx"      "(" ID ")"
            | "io.lora.beacon"  "(" ID ")"
            | "io.debug.dump"   "(" ID ")"
bron-soort  = "rf" | "usb" | "lora" | "programmatic"
            | "timer" "(" GETAL ")"
```

---

## Bronverklaringen

```rivr
source NAAM [@KLOK] = SOORT;
```

| Parameter | Beschrijving |
|---|---|
| `NAAM` | Identifier om naar deze bron te verwijzen |
| `@KLOK` | Optionele klokannotatie (`@mono`, `@lmp`) |
| `SOORT` | Hardware-oorsprong: `usb`, `lora`, `rf`, `programmatic`, of `timer(N)` |

**Bronsoorten:**

| Soort | Beschrijving |
|---|---|
| `rf` | LoRa radio-ontvangststroom — events zijn ruwe `Bytes`-frames |
| `usb` | USB/UART-ontvangststroom |
| `lora` | Alias voor `rf` |
| `programmatic` | Handmatig geïnjecteerde events (klok 0) |
| `timer(N)` | Periodieke tik elke N milliseconden (klok 0). Vuurt automatisch `Value::Int(mono_ms)`-events vanuit de C-timertabel. |

**Klok-IDs:**

| Naam | ID | Beschrijving |
|---|---|---|
| `mono` | 0 | Monotone milliseconde-wandklok |
| `lmp`  | 1 | Lamport-klok (LoRa-mesh) |

**Voorbeelden:**

```rivr
source rf_rx @lmp  = rf;           // LoRa-ontvangst → Lamport-klok
source usb   @mono = usb;          // USB-stroom → mono-klok
source sensor      = programmatic;  // klok 0 is standaard
source beacon_tick = timer(30000);  // vuurt elke 30 s, klok 0
```

---

## Pijp-operatoren

### Tekst / transformatie

| Operator | Beschrijving |
|---|---|
| `map.upper()` | Converteer `Str`-payload naar ASCII-hoofdletters |
| `map.lower()` | Converteer `Str`-payload naar ASCII-kleine letters |
| `map.trim()` | Verwijder voor- en achterliggende ASCII-witruimte van `Str`-payload |
| `filter.nonempty()` | Gooi events met lege of spatie-only `Str`-payload weg |

### Filteren

| Operator | Beschrijving |
|---|---|
| `filter.kind("TAG")` | Laat alleen events toe waarvan de string-payload begint met `"TAG:"`. Gebruik `"*"` om alles door te laten. |
| `filter.pkt_type(N)` | Laat alleen `Bytes`-events toe waarbij byte op offset 3 gelijk is aan `N` (RIVR binaire pakket-header). |

Pakket-type constanten voor `filter.pkt_type`:

| Constante | Waarde | Betekenis |
|---|---|---|
| `PKT_CHAT` | 1 | Tekstbericht |
| `PKT_BEACON` | 2 | Node-aanwezigheidsadvertentie |
| `PKT_ROUTE_REQ` | 3 | Routeverzoek |
| `PKT_ROUTE_RPL` | 4 | Route-antwoord |
| `PKT_ACK` | 5 | Bevestiging |
| `PKT_DATA` | 6 | Generieke sensordata |
| `PKT_PROG_PUSH` | 7 | OTA-programma-update (nooit doorgestuurd) |
| `PKT_TELEMETRY` | 8 | Gestructureerde sensorlezing (11-byte vaste payload) |
| `PKT_MAILBOX` | 9 | Store-and-forward bericht (7-byte header + tekstinhoud) |
| `PKT_ALERT` | 10 | Prioriteitsmeldingsgebeurtenis (7-byte vaste payload) |

### Aggregatie

| Operator | Beschrijving |
|---|---|
| `fold.count()` | Vervang elk event door een lopende integerteller (start bij 1) |
| `fold.sum()` | Vervang elk event door de lopende som van alle `Int`-payloads |
| `fold.last()` | Zend de waarde van het *vorige* event (stuurt `Unit` vóór het eerste event) |

### Vensters (ms-domein, klok 0)

| Operator | Beschrijving |
|---|---|
| `window.ms(N)` | Kantelend venster van N milliseconden; zendt een `Window` (lijst van strings) bij elke grens |
| `throttle.ms(N)` | Stuur maximaal één event per N ms door |
| `debounce.ms(N)` | Stuur het wachtende event pas door na N ms stilte |

### Vensters (tik-domein)

| Operator | Beschrijving |
|---|---|
| `window.ticks(N)` | Kantelend venster van N tikken |
| `window.ticks(N, CAP, "BELEID")` | Begrensd venster met overloopbeleid |
| `throttle.ticks(N)` | Stuur maximaal één event per N tikken door |
| `delay.ticks(N)` | Vertraag doorsturen met N tikken |

**Vensteroverloopbeleidsregels:**

| Beleid | Gedrag wanneer buffer CAP bereikt vóór tikgrens |
|---|---|
| `"drop_oldest"` | Verwijder oudste event, accepteer nieuw event |
| `"drop_newest"` | Gooi nieuw event weg, bewaar bestaande buffer |
| `"flush_early"` | Zend volledige buffer direct als `Window`, accepteer dan nieuw event |

### Snelheidsbegrenzing

| Operator | Beschrijving |
|---|---|
| `budget(RATE, BURST)` | Generiek token-emmer: RATE events/s, BURST-capaciteit |
| `budget.airtime(VENSTER_TIKKEN, DUTY)` | Radio duty-cycle op basis van aantal events |
| `budget.toa_us(VENSTER_MS, DUTY, TOA_US)` | Radio duty-cycle op basis van werkelijke Time-on-Air |

**`budget.toa_us`-parameters:**

| Parameter | Beschrijving |
|---|---|
| `VENSTER_MS` | Meetvenster in tikken (= ms op klok 0) |
| `DUTY` | Maximale fractie van venster voor TX (0,0–1,0) |
| `TOA_US` | Luchttijdkosten per event in microseconden |

**Voorbeeld** — SF9 BW125 kHz, 50-byte payload, 10% duty over 6 minuten:
```rivr
|> budget.toa_us(360000, 0.10, 360000)
```
Budget: `360 000 × 0,10 × 1 000 = 36 000 000 µs` per venster.
Max pakketten: `36 000 000 / 360 000 = 100`.

### Fan-in

```rivr
let gecombineerd = merge(bron_a, bron_b);
```

Voegt twee streams samen tot één (events van beide passeren).

### Debug

| Operator | Beschrijving |
|---|---|
| `tag("LABEL")` | Voeg een trace-label toe aan elk passerend event |

---

## Emit / sinks

```rivr
emit {
  io.lora.tx(STREAM);
  io.usb.print(STREAM);
  io.debug.dump(STREAM);
}
```

| Sink | C-callback | Gedrag |
|---|---|---|
| `io.lora.tx` | `rf_tx_sink_cb` | Codeer en zet in `rf_tx_queue` |
| `io.lora.beacon` | `beacon_sink_cb` | Bouw `PKT_BEACON` (roepnaam + hop_count) en zet in `rf_tx_queue` |
| `io.usb.print` | `usb_print_sink_cb` | `printf` naar UART-stdout |
| `io.debug.dump` | `log_sink_cb` | ESP-IDF `ESP_LOGI` |

---

## Volledige voorbeelden

### Standaard mesh-pijplijn (met baken)

```rivr
source rf_rx @lmp = rf;
source beacon_tick = timer(30000);

let chat = rf_rx
  |> filter.pkt_type(1)
  |> budget.toa_us(280000, 0.10, 280000)
  |> throttle.ticks(1);

emit { io.lora.tx(chat); }
emit { io.lora.beacon(beacon_tick); }
```

### Multi-type mesh-routing

```rivr
source rf_rx @lmp = rf;
source beacon_tick = timer(30000);

let chat = rf_rx
  |> filter.pkt_type(1)
  |> budget.toa_us(280000, 0.10, 280000)
  |> throttle.ticks(1);

let data = rf_rx
  |> filter.pkt_type(6)
  |> throttle.ticks(1);

emit { io.lora.tx(chat); }
emit { io.lora.tx(data); }
emit { io.lora.beacon(beacon_tick); }
```

### Begrensd venster met vroeg-doorzenden

```rivr
source rf @lmp = rf;

let burst = rf
  |> window.ticks(10, 3, "flush_early");

emit { io.usb.print(burst); }
```

### Multi-bron samenvoegen

```rivr
source usb  = usb;
source lora = lora;

let beide = merge(usb, lora)
  |> map.upper()
  |> map.trim()
  |> filter.nonempty();

emit { io.usb.print(beide); }
```

---

## Parser-regels en randgevallen

- Commentaar: `// alleen eenregelig`
- Identifiers: ASCII alfanumeriek + underscore
- Tekenreeksliteralen: dubbele aanhalingstekens, ondersteunt `\\`, `\"`, `\n`, `\t`
- Gehele getallen: alleen decimaal (geen `0x` hex in RIVR-broncode)
- Kommagetallen: eenvoudig decimaal (bijv. `0.10`, niet `10%`)
- `filter.pkt_type(N)`: N moet 0–255 zijn
- Operatoren zijn punt-naamruimte-gebonden: `budget.toa_us`, `filter.pkt_type`, enz.
- Het `|>`-token mag geen spatie hebben tussen `|` en `>`
