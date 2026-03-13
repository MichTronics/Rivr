/**
 * @file  rivr_iface_usb.c
 * @brief USB-UART SLIP bridge transport adapter.
 *
 * Only compiled when RIVR_FEATURE_USB_BRIDGE=1.  When disabled, the header
 * provides inline no-op stubs so no code is generated for this translation unit.
 */

#include "rivr_iface_usb.h"

#if RIVR_FEATURE_USB_BRIDGE

#include "firmware_core/radio_sx1262.h"   /* rf_rx_ringbuf, rf_rx_frame_t    */
#include "firmware_core/rivr_bus/rivr_bus_types.h"  /* RIVR_IFACE_USB        */
#include "firmware_core/ringbuf.h"         /* rb_try_push                    */
#include "firmware_core/timebase.h"        /* tb_millis()                    */
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>

#define TAG "USB_IFACE"

/* ── SLIP framing constants (RFC 1055) ──────────────────────────────────── */
#define SLIP_END      0xC0u  /* Frame boundary marker                         */
#define SLIP_ESC      0xDBu  /* Escape byte                                   */
#define SLIP_ESC_END  0xDCu  /* Escaped END (0xC0 inside payload → DB DC)     */
#define SLIP_ESC_ESC  0xDDu  /* Escaped ESC (0xDB inside payload → DB DD)     */

/* ── UART RX parameters ──────────────────────────────────────────────────── */
#define USB_UART_RX_BUF 512u   /* UART driver RX ring-buffer (bytes)          */
#define USB_UART_TX_BUF   0u   /* TX: 0 = blocking (write blocks until done)  */
#define USB_DRAIN_CHUNK 256u   /* Max bytes read per rivr_iface_usb_tick()    */

/* ── SLIP RX decoder state ───────────────────────────────────────────────── */
typedef struct {
    uint8_t buf[RF_MAX_PAYLOAD_LEN]; /* Assembling decoded frame              */
    uint8_t len;                     /* Bytes written into buf so far         */
    bool    esc;                     /* Previous byte was SLIP_ESC            */
} slip_rx_t;

static bool     s_usb_ready = false;
static slip_rx_t s_slip_rx  = {0};

/* ── Internal: feed one decoded byte to the assembler ───────────────────── */

/**
 * @brief Feed a raw SLIP byte into the RX state machine.
 *
 * Handles SLIP_END (frame complete), SLIP_ESC (set escape flag), and payload
 * bytes (escaped or literal).  Does NOT write invalid escape sequences into
 * the assembly buffer — they are silently discarded to avoid corrupt frames.
 *
 * @param b       The byte just received from the UART
 * @param out_len Set to assembled frame length when returning true
 * @return true when a complete frame is ready in s_slip_rx.buf
 */
static bool slip_feed(uint8_t b, uint8_t *out_len)
{
    if (b == SLIP_END) {
        /* Frame boundary — accept only if we have at least 1 byte */
        if (s_slip_rx.len > 0u) {
            *out_len        = s_slip_rx.len;
            s_slip_rx.len   = 0u;
            s_slip_rx.esc   = false;
            return true;
        }
        /* Leading END (common at start of stream) — ignore */
        return false;
    }

    if (s_slip_rx.esc) {
        s_slip_rx.esc = false;
        if (b == SLIP_ESC_END) {
            b = SLIP_END;
        } else if (b == SLIP_ESC_ESC) {
            b = SLIP_ESC;
        } else {
            /* Invalid escape — discard assembled fragment, reset */
            ESP_LOGW(TAG, "SLIP: bad escape 0x%02x, discarding fragment", b);
            s_slip_rx.len = 0u;
            return false;
        }
    } else if (b == SLIP_ESC) {
        s_slip_rx.esc = true;
        return false;
    }

    /* Append to assembly buffer if space remains */
    if (s_slip_rx.len < RF_MAX_PAYLOAD_LEN) {
        s_slip_rx.buf[s_slip_rx.len++] = b;
    } else {
        /* Buffer overflow — discard and reset */
        ESP_LOGW(TAG, "SLIP: frame too large (>%u bytes), discarding", RF_MAX_PAYLOAD_LEN);
        s_slip_rx.len = 0u;
        s_slip_rx.esc = false;
    }
    return false;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void rivr_iface_usb_init(void)
{
    const uart_config_t cfg = {
        .baud_rate           = RIVR_USB_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    esp_err_t err;

    err = uart_param_config((uart_port_t)RIVR_USB_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %d", err);
        return;
    }

    err = uart_set_pin((uart_port_t)RIVR_USB_UART_NUM,
                       RIVR_USB_UART_TX, RIVR_USB_UART_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %d", err);
        return;
    }

    err = uart_driver_install((uart_port_t)RIVR_USB_UART_NUM,
                              USB_UART_RX_BUF, USB_UART_TX_BUF,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %d", err);
        return;
    }

    s_usb_ready = true;
    ESP_LOGI(TAG, "USB-UART bridge ready: UART%d TX=%d RX=%d baud=%d",
             RIVR_USB_UART_NUM, RIVR_USB_UART_TX, RIVR_USB_UART_RX,
             RIVR_USB_UART_BAUD);
}

bool rivr_iface_usb_ready(void)
{
    return s_usb_ready;
}

bool rivr_iface_usb_send(const uint8_t *data, size_t len)
{
    if (!s_usb_ready || !data || len == 0u || len > RF_MAX_PAYLOAD_LEN) {
        return false;
    }

    /* SLIP-encode into a local stack buffer.
     * Worst case: every byte is 0xC0 or 0xDB → 2× payload + 2 END bytes.
     * RF_MAX_PAYLOAD_LEN = 255 → worst-case 512 payload + 2 END = 514 bytes. */
    uint8_t  slip_buf[RF_MAX_PAYLOAD_LEN * 2u + 2u];
    uint16_t out = 0u;

    slip_buf[out++] = SLIP_END;  /* leading END clears any buffered junk */

    for (size_t i = 0u; i < len; i++) {
        if (data[i] == SLIP_END) {
            slip_buf[out++] = SLIP_ESC;
            slip_buf[out++] = SLIP_ESC_END;
        } else if (data[i] == SLIP_ESC) {
            slip_buf[out++] = SLIP_ESC;
            slip_buf[out++] = SLIP_ESC_ESC;
        } else {
            slip_buf[out++] = data[i];
        }
    }

    slip_buf[out++] = SLIP_END;  /* trailing END */

    int written = uart_write_bytes((uart_port_t)RIVR_USB_UART_NUM,
                                   slip_buf, (size_t)out);
    return (written == (int)out);
}

void rivr_iface_usb_tick(void)
{
    if (!s_usb_ready) {
        return;
    }

    uint8_t  raw[USB_DRAIN_CHUNK];
    int      avail = uart_read_bytes((uart_port_t)RIVR_USB_UART_NUM,
                                     raw, USB_DRAIN_CHUNK, 0 /* non-blocking */);
    if (avail <= 0) {
        return;
    }

    uint32_t now_ms = tb_millis();

    for (int i = 0; i < avail; i++) {
        uint8_t frame_len = 0u;
        if (slip_feed(raw[i], &frame_len)) {
            /* Complete SLIP frame assembled in s_slip_rx.buf */
            rf_rx_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            if (frame_len > RF_MAX_PAYLOAD_LEN) {
                continue;  /* sanity (slip_feed already guards this) */
            }
            memcpy(frame.data, s_slip_rx.buf, frame_len);
            frame.len        = frame_len;
            frame.rssi_dbm   = 0;     /* no RSSI for USB frames */
            frame.snr_db     = 0;
            frame.rx_mono_ms = now_ms;
            frame.from_id    = 0u;    /* unknown — USB bridge doesn't know hop */
            frame.iface      = (uint8_t)RIVR_IFACE_USB;

            if (!rb_try_push(&rf_rx_ringbuf, &frame)) {
                ESP_LOGW(TAG, "USB rx: rf_rx_ringbuf full, frame dropped");
            }
        }
    }
}

#endif /* RIVR_FEATURE_USB_BRIDGE */
