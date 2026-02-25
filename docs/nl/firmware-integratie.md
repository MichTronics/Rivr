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

**Hoofdluspatroon (uit `rivr_embed.c`):**

```c
void rivr_tick(void) {
    sources_rf_rx_drain();   // injecteer radio-frames
    sources_cli_drain();     // injecteer CLI-events
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
