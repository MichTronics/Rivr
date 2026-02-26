# RIVR — Firmware-integratie

Deze handleiding beschrijft hoe je `rivr_core` (de Rust statische bibliotheek)
integreert in een C-firmwareproject — zowel met de meegeleverde ESP32-laag als
op een nieuw platform.

---

## C-header: `rivr_embed.h`

Voeg dit bestand in elk C-vertaaleenheid dat RIVR aanroept:

```c
#include "rivr_embed.h"
```

---

## Waardetypen

```c
/* Tag-byte opgeslagen in rivr_value_t.tag */
#define RIVR_VAL_UNIT   0
#define RIVR_VAL_INT    1
#define RIVR_VAL_BOOL   2
#define RIVR_VAL_STR    3
#define RIVR_VAL_BYTES  4
#define RIVR_VAL_WINDOW 5

typedef struct {
    uint8_t  tag;        /* RIVR_VAL_*-constante */
    uint32_t len;        /* bytelengte van data   */
    uint8_t  data[256];  /* payload               */
} rivr_value_t;
```

`RIVR_VAL_BYTES` wordt gebruikt voor ruwe LoRa-frames.
`filter.pkt_type(N)` controleert `value.data[3] == N` (offset = `PKT_TYPE_BYTE_OFFSET`).

---

## Event-type

```c
typedef struct {
    uint8_t       source_id;  /* welke bron (0-gebaseerde index) */
    uint8_t       clock_id;   /* 0 = mono, 1 = lmp               */
    uint64_t      timestamp;  /* ms (mono) of tikken (lmp)       */
    rivr_value_t  value;
} rivr_event_t;
```

---

## Levenscyclus-API

### 1 — Initialiseer de engine

```c
rivr_result_t rivr_engine_init(const char *program);
```

`program` is een NUL-afgeëindigd RIVR-broncode-string.
`rivr_embed_init()` probeert automatisch eerst NVS; het ingebakken
`RIVR_ACTIVE_PROGRAM` wordt als terugval gebruikt bij eerste opstart.

| `rc.code` | Betekenis |
|---|---|
| `RIVR_OK` | Succes — engine gereed |
| `RIVR_ERR_PARSE` | Parsefout in bronstring |
| `RIVR_ERR_COMPILE` | Compilatie-/semantische fout |

**Voorbeeld:**

```c
rivr_result_t rc = rivr_engine_init(RIVR_DEFAULT_PROGRAM);
if (rc.code != RIVR_OK) {
    ESP_LOGE(TAG, "RIVR init mislukt: %" PRIu32, rc.code);
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
```

### 2 — Registreer de emit-dispatcher

```c
typedef void (*rivr_emit_fn)(uint8_t sink_id,
                             const rivr_value_t *value);

void rivr_set_emit_dispatch(rivr_emit_fn fn);
```

Registreer je dispatcher **vóór** de eerste `rivr_engine_run()`-aanroep.
`sink_id` is de nulgebaseerde index van de `emit { … }`-clausule die is gevuurd.

**Voorbeeld-dispatcher (ESP32):**

```c
static void emit_dispatch(uint8_t sink_id, const rivr_value_t *v) {
    if (sink_id == SINK_RF_TX && v->tag == RIVR_VAL_BYTES) {
        rf_enqueue(v->data, v->len);
    } else if (sink_id == SINK_USB_PRINT && v->tag == RIVR_VAL_STR) {
        printf("[RIVR] %.*s\n", (int)v->len, v->data);
    }
}

rivr_set_emit_dispatch(emit_dispatch);
```

### 3 — Injecteer events

```c
void rivr_inject_event(const rivr_event_t *event);
```

Roep eenmaal aan per ontvangen frame of sensormeting.
Veilig vanuit elke taak; het event wordt direct verwerkt (geen interne wachtrij).

**LoRa RX-voorbeeld:**

```c
rivr_event_t ev = {
    .source_id = SOURCE_RF_RX,
    .clock_id  = 1,                  /* lmp */
    .timestamp = lmp_clock_now(),
    .value = {
        .tag = RIVR_VAL_BYTES,
        .len = frame_len,
    }
};
memcpy(ev.value.data, frame_buf, frame_len);
rivr_inject_event(&ev);
```

### 4 — Klok vooruitschuiven / tikoperatoren

```c
rivr_result_t rivr_engine_run(uint32_t max_steps);
```

Elke hoofdlustitratie aangeroepen vanuit `rivr_tick()`.

**Hoofdluspatroon (uit `rivr_embed.c` / `main.c`):**

```c
// rivr_cli_poll() MÖET als eerste komen zodat UART0-bytes worden gelezen
// vóór rivr_tick() sources_cli_drain() aanroept.
void main_loop_body(void) {
#if RIVR_ROLE_CLIENT
    rivr_cli_poll();         // interactieve seriële CLI (cliënt-builds)
#endif
    radio_service_rx();      // lees DIO1-events via SPI (alleen hoofdtaak)
    rivr_tick();             // ring-buffer → engine → emit
    tx_drain_loop();         // verstuur TX-frames met duty-cycle-bewaker
}

void rivr_tick(void) {
    sources_rf_rx_drain();   // injecteer radio-frames
    sources_cli_drain();     // injecteer CLI-events (no-op op echte hardware)
    sources_timer_drain();   // vuur vervallen timer-bronnen
    rivr_engine_run(256);    // voer DAG uit
}
```

### 5 — NVS-programmaopslag

```c
bool rivr_nvs_store_program(const char *src);
```

Slaat `src` op in NVS-sleutel `rivr/program` (max. 2047 bytes). Bij de volgende
opstart (of na `rivr_embed_reload()`) wordt dit programma gebruikt in plaats van
de ingebakken standaard.

```c
bool rivr_embed_reload(void);
```

Herlaadt de engine op levende firmware: reset de timertabel, initialiseert de
engine opnieuw vanuit NVS (of terugval), en telt timer-bronnen opnieuw.
Wordt automatisch aangeroepen vanuit de hoofdlus als `g_program_reload_pending`
is gezet (bijv. na ontvangst van een `PKT_PROG_PUSH`-frame).

### 6 — Klokken bevragen

```c
uint64_t rivr_engine_clock_now(uint8_t clock_id);
```

Geeft de huidige tijdstempel terug voor het opgegeven klok-ID (0 = mono, 1 = lmp)
uit de interne status van de engine na de laatste `rivr_engine_run()`-aanroep.

---

## Sink-registratie

Sinks worden op naam geregistreerd in `rivr_sinks_init()` via `rivr_register_sink()`:

```c
rivr_register_sink("io.lora.tx",     rf_tx_sink_cb,     NULL);
rivr_register_sink("io.lora.beacon", beacon_sink_cb,    NULL);
rivr_register_sink("io.usb.print",   usb_print_sink_cb, NULL);
rivr_register_sink("io.debug.dump",  log_sink_cb,       NULL);
```

`beacon_sink_cb` bouwt een `PKT_BEACON`-frame met de roepnaam (`g_callsign`),
net-ID (`g_net_id`) en hop count, en plaatst dit in `rf_tx_queue`.

---

## Binair protocol

Frames op het netwerk volgen het RIVR binaire pakketformaat zoals gedefinieerd in
`firmware_core/protocol.h`.

### Header-indeling

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;       /* 0xR1 */
    uint8_t  version;     /* protocolversie */
    uint8_t  hop_limit;   /* verlaagd per hop */
    uint8_t  pkt_type;    /* PKT_CHAT=1 … PKT_DATA=6 */
    uint32_t src_node_id; /* herkomstnode */
    uint32_t seq_no;      /* volgnummer per bron */
    uint8_t  payload[];   /* variabele payload */
} rivr_pkt_hdr_t;
```

### Pakket-types

| Constante | Waarde |
|---|---|
| `PKT_CHAT` | 1 |
| `PKT_BEACON` | 2 |
| `PKT_ROUTE_REQ` | 3 |
| `PKT_ROUTE_RPL` | 4 |
| `PKT_ACK` | 5 |
| `PKT_DATA` | 6 |
| `PKT_PROG_PUSH` | 7 |

`PKT_PROG_PUSH` bevat een NUL-afgeëindigd RIVR-broncode-string als payload.
Bij ontvangst slaat de firmware dit op in NVS en zet `g_program_reload_pending`.
Het wordt **nooit doorgestuurd** over het mesh-netwerk.

### Coderen / decoderen

```c
/* Codeer een uitgaande struct → bytes (geeft aantal geschreven bytes terug) */
int protocol_encode(const rivr_pkt_hdr_t *hdr,
                    const uint8_t *payload, size_t payload_len,
                    uint8_t *out_buf,       size_t out_cap);

/* Decodeer een inkomende byte-buffer → struct */
int protocol_decode(const uint8_t *buf,  size_t len,
                    rivr_pkt_hdr_t *hdr,
                    uint8_t *payload_out, size_t payload_cap);
```

Beide functies geven `0` bij succes en een negatieve foutcode bij mislukking.
`protocol_decode` valideert de CRC-16 die aan elk frame is toegevoegd.

---

## Routing-API

`firmware_core/routing.h` biedt lichtgewicht deduplicatie en doorstuurbeslissingen
zonder heap-allocatie.

```c
/* Geeft niet-nul terug als dit (src, seq_no)-paar recentelijk al gezien is */
int routing_dedupe_check(uint32_t src_node_id, uint32_t seq_no);

/* Geeft niet-nul terug als het frame doorgestuurd moet worden op basis van hop_limit */
int routing_should_forward(const rivr_pkt_hdr_t *hdr);

/* Werk de buurttabel bij met signaalsterkte-informatie */
void routing_update_neighbour(uint32_t node_id, int8_t rssi, int8_t snr);
```

Typisch gebruik in de LoRa RX-onderbrekingsroutine / hoofdlus:

```c
rivr_pkt_hdr_t hdr;
uint8_t payload[128];

if (protocol_decode(raw, raw_len, &hdr, payload, sizeof(payload)) < 0)
    return;                                           // slechte CRC → weggooien

if (routing_dedupe_check(hdr.src_node_id, hdr.seq_no))
    return;                                           // duplicaat → weggooien

if (routing_should_forward(&hdr))
    rf_enqueue(raw, raw_len);                         // opnieuw uitzenden

rivr_inject_event(&(rivr_event_t){ ... });            // voer lokale pijplijn
```

---

## Rivr Fabric-API

`firmware_core/rivr_fabric.h` biedt congestie-bewuste relaybeslissingen.
De module is alleen actief wanneer `RIVR_FABRIC_REPEATER=1`; alle functies
zijn anders no-ops.

### Levenscyclus

```c
void rivr_fabric_init(void);            // eenmalig aanroepen in app_main na platform_init
void rivr_fabric_tick(uint32_t now_ms); // elke hoofdlustitratie aanroepen
```

### Verkeersmeldingen (via radio-/TX-pad)

```c
void rivr_fabric_on_rx(void);            // na elk ontvangen frame
void rivr_fabric_on_tx_enqueued(void);   // relay-frame in TX-wachtrij geplaatst
void rivr_fabric_on_tx_ok(void);         // TX succesvol afgerond
void rivr_fabric_on_tx_fail(void);       // TX mislukt (time-out / PA-fout)
void rivr_fabric_on_tx_blocked_dc(void); // TX geblokkeerd door duty-cycle-bewaker
```

### Relaybeslissing

```c
fabric_decision_t rivr_fabric_decide_relay(uint8_t pkt_type, uint32_t now_ms);
```

Geeft een van de volgende waarden terug:

| Waarde | Betekenis |
|---|---|
| `FABRIC_SEND_NOW` | Relay onmiddellijk |
| `FABRIC_DELAY` | Voeg jitter-vertraging toe (score ≥ `RIVR_FABRIC_DELAY_THRESHOLD`) |
| `FABRIC_DROP` | Onderdruk relay volledig (score ≥ `RIVR_FABRIC_DROP_THRESHOLD`) |

Alleen `PKT_CHAT` en `PKT_DATA` worden gepoort; alle overige typen geven
altijd `FABRIC_SEND_NOW` terug.  Wanneer de score
`RIVR_FABRIC_BLACKOUT_GUARD_SCORE` bereikt, wordt het resultaat begrensd
tot `FABRIC_DELAY` (maximale vertraging) om een volledige relay-blackout
te voorkomen.

### Debug / display

```c
typedef struct {
    uint8_t  score;           // 0–100 congestiescore
    uint8_t  band;            // 0=OK 1=LICHT_VTG 2=VTG 3=DROP
    uint32_t rx_per_s;        // ontvangen frames gemiddeld over 60 s
    uint32_t blocked_per_s;   // duty-cycle-blokkades gemiddeld over venster
    uint32_t fail_per_s;      // TX-fouten gemiddeld over venster
    uint32_t relay_drop_total;
    uint32_t relay_delay_total;
} fabric_debug_t;

void rivr_fabric_get_debug(uint32_t now_ms, fabric_debug_t *out);
```

### Scalaire getters

```c
uint8_t  rivr_fabric_get_score(void);
uint32_t rivr_fabric_get_relay_delayed(void);
uint32_t rivr_fabric_get_relay_dropped(void);
uint32_t rivr_fabric_get_relay_total(void);
uint32_t rivr_fabric_get_tx_blocked(void);
uint32_t rivr_fabric_get_rx_total(void);
```

### Instelbare drempelwaarden (allemaal `#ifndef`-bewaakt)

| Macro | Standaard | Beschrijving |
|---|---|---|
| `RIVR_FABRIC_DROP_THRESHOLD` | `80` | Score ≥ waarde → DROP |
| `RIVR_FABRIC_DELAY_THRESHOLD` | `50` | Score ≥ waarde → DELAY |
| `RIVR_FABRIC_LIGHT_DELAY_THRESHOLD` | `20` | Score ≥ waarde → korte jitter |
| `RIVR_FABRIC_MAX_EXTRA_DELAY_MS` | `1000` | Maximale DELAY in milliseconden |
| `RIVR_FABRIC_BLACKOUT_GUARD_SCORE` | `95` | Begrens DROP tot DELAY vanaf deze score |

### Periodiek logbericht

Wanneer `RIVR_FABRIC_REPEATER=1` logt `main.c` elke 5 s een `[FAB]`-regel:

```
I (...) MAIN: [FAB] score=34 rx=12 relay=8 delay=2 drop=0 dc_block=1
```

---

## Knoopvarianten

RIVR ondersteunt compilatietijd-rolselectie via variantheaders.  Elke
variantheader staat in `variants/<board>/config.h` en wordt door PlatformIO
via `-include` ge\u00efnjecteerd.  Elke macro is omsloten door `#ifndef` zodat
een `-D`-vlag altijd wint.

### Beschikbare rols

| Macro | Rol |
|---|---|
| *(geen)* | Standaardknoop \u2014 volledige relay van alle pakkettypen |
| `RIVR_BUILD_REPEATER=1` + `RIVR_FABRIC_REPEATER=1` | Repeater \u2014 volledige relay met Fabric-congestiepoort voor `PKT_CHAT`/`PKT_DATA` |
| `RIVR_ROLE_CLIENT=1` | Cli\u00ebnt \u2014 ontvangt `PKT_CHAT`/`PKT_DATA` lokaal, relay wordt onderdrukt; bestuurpakketten worden normaal doorgestuurd |

### Relay-onderdrukking voor cli\u00ebnten

In `rivr_layer/rivr_sources.c` zorgt de volgende bewaker ervoor dat
relay-frames voor chat en data worden overgeslagen bij een cli\u00ebnt-build:

```c
#if RIVR_ROLE_CLIENT
    if (fwd_hdr.pkt_type == PKT_CHAT || fwd_hdr.pkt_type == PKT_DATA) {
        goto skip_enqueue;   // relay onderdrukken; alleen lokaal afleveren
    }
#endif
```

Bestuurpakketten (`PKT_BEACON`, `PKT_ROUTE_REQ`, `PKT_ROUTE_RPL`, `PKT_ACK`,
`PKT_PROG_PUSH`) worden niet geïnvloed en worden normaal doorgestuurd zodat
mesh-routering intact blijft.

### Seriële CLI (`RIVR_ROLE_CLIENT=1`)

`rivr_layer/rivr_cli.h` biedt een API met drie functies.  In alle niet-cliënt
builds compileert elke functie tot een zero-cost inline no-op.

```c
#include "rivr_layer/rivr_cli.h"

void rivr_cli_init(void);
// Eenmalig aanroepen vanuit app_main() na rivr_embed_init().
// Drukt de opstartbanner en de begin-"> "-prompt af.

void rivr_cli_poll(void);
// Aanroepen als EERSTE regel van de hoofdlus (vóór radio_service_rx()).
// Niet-blokkerend: leest UART0 RX-bytes, echoet invoer, verwerkt BS/DEL.
// Bij newline worden de volgende commando’s afgehandeld:
//   chat <bericht>  — bouw + zet PKT_CHAT in wachtrij; print "TX CHAT: <bericht>"
//   id              — druk node-ID en net-ID af
//   help            — toon commando’s

void rivr_cli_on_chat_rx(uint32_t src_id,
                          const uint8_t *payload, uint8_t len);
// Aangeroepen vanuit rivr_sources.c (sectie 6b) na decodering van PKT_CHAT.
// Drukt af: "\r[CHAT][XXXXXXXX]: <bericht>\n> "
// Het \r wist de gedeeltelijke promptregel; "> " herstartte de prompt.
```

**UART-driver-opmerking** — `rivr_cli_init()` roept `uart_is_driver_installed()`
aan vóór `uart_driver_install()`, zodat het veilig is te roepen zelfs als de
VFS-console de driver al heeft geïnstalleerd.  De poll-functie gebruikt
`uart_read_bytes(UART_NUM_0, &ch, 1, 0)` met een nul-time-out voor volledig
niet-blokkerende werking.  Maximale berichtlengte is `RIVR_PKT_MAX_PAYLOAD`
(231 bytes); de regelsbuffer is 128 bytes, dus overloop-tekens worden stil
verworpen.

---

## Simulatiemodus

Compileertime-vlaggen die echte hardware vervangen:

| Vlag | Waarde | Effect |
|---|---|---|
| `RIVR_SIM_MODE` | `1` | Sla `platform_init()` over; roep `radio_init_buffers_only()` aan |
| `RIVR_SIM_TX_PRINT` | `1` | Print `[SIM TX]` naar UART in plaats van via SX1262 te zenden |

Instellen via PlatformIO:

```ini
[env:esp32_sim]
build_flags = -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1
```

Of via idf.py:

```powershell
idf.py -DRIVR_SIM_MODE=1 -DRIVR_SIM_TX_PRINT=1 build
```

---

## Porteer-checklist (nieuw platform)

1. Implementeer `platform_init()` en `platform_clock_ms()` voor jouw MCU.
2. Bied een UART-driver en koppel deze aan `usb_print_sink_cb`.
3. Bied een radio-driver met `radio_transmit(buf, len)` en een ontvangst-ring-buffer.
4. Koppel `librivr_core.a` (Rust, cross-gecompileerd voor jouw doel).
5. Roep de levenscyclus aan: `rivr_embed_init()` → hoofdlus met `rivr_tick()` + `tx_drain_loop()`.
6. Optioneel: roep `rivr_nvs_store_program()` aan om gebruikersprogramma's op te slaan.
