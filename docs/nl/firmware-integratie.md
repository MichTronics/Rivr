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
int rivr_engine_init(const char *program, size_t len);
```

| Retourwaarde | Betekenis |
|---|---|
| `0` | Succes — engine gereed |
| `-1` | Parsefout |
| `-2` | Compilatiefout |

`program` is een NUL-afgeëindigd RIVR-broncode-string.
In de ESP32-build wordt het actieve programma geselecteerd via `RIVR_ACTIVE_PROGRAM` in `default_program.h`.

**Voorbeeld:**

```c
static const char PROG[] =
    "source rf_rx @lmp = rf_rx;\n"
    "let chat = rf_rx |> filter.pkt_type(1) |> throttle.ticks(1);\n"
    "emit { rf_tx(chat); }\n";

if (rivr_engine_init(PROG, strlen(PROG)) != 0) {
    ESP_LOGE(TAG, "RIVR init mislukt");
    abort();
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
void rivr_engine_run(uint64_t clock0_ms, uint64_t clock1_ticks);
```

Roep elke **hoofdlustitratie** aan om tijdgebaseerde operatoren te tikken
(`window.ms`, `throttle.ms`, `debounce.ms`, enz.).

**Hoofdluspatroon:**

```c
while (1) {
    uint64_t nu_ms    = esp_timer_get_time() / 1000;
    uint64_t nu_tikken = lmp_clock_now();
    rivr_engine_run(nu_ms, nu_tikken);
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

### 5 — Klokken bevragen

```c
uint64_t rivr_engine_clock_now(uint8_t clock_id);
```

Geeft de huidige tijdstempel terug voor het opgegeven klok-ID (0 = mono, 1 = lmp)
uit de interne status van de engine na de laatste `rivr_engine_run()`-aanroep.

---

## Sink-registratie-helper

`rivr_layer/rivr_sources.c` biedt handige macro's:

```c
rivr_register_sink(SINK_RF_TX,     rf_tx_sink_cb);
rivr_register_sink(SINK_USB_PRINT, usb_print_sink_cb);
rivr_register_sink(SINK_LOG,       log_sink_cb);
```

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
5. Roep de levenscyclus aan: `rivr_engine_init` → `rivr_set_emit_dispatch` → loop `rivr_inject_event` + `rivr_engine_run`.
