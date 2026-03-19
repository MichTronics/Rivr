/**
 * @file  firmware_core/radio_sx1262_rp2040.c
 * @brief SX1262 radio driver for RP2040 — replaces radio_sx1262.c.
 *
 * Functionally identical to radio_sx1262_nrf52.c but adapted for RP2040
 * (arduino-pico, no FreeRTOS):
 *
 *   taskENTER_CRITICAL / taskEXIT_CRITICAL → noInterrupts() / interrupts()
 *   vTaskDelay(pdMS_TO_TICKS(1))           → delay(1)
 *   FreeRTOS includes                      → removed
 *
 * Only compiled when RIVR_PLATFORM_RP2040 is defined.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "radio_sx1262.h"
#include "platform_rp2040.h"
#include "timebase.h"
#include "rivr_metrics.h"
#include "rivr_log.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <Arduino.h>

/* ── Critical section ─────────────────────────────────────────────────────
 * arduino-pico provides noInterrupts() / interrupts() which disable/enable
 * all IRQs on Cortex-M0+ — functionally equivalent to portDISABLE_INTERRUPTS.
 */
#define RADIO_ENTER_CRITICAL()  noInterrupts()
#define RADIO_EXIT_CRITICAL()   interrupts()

/* ── Logging ──────────────────────────────────────────────────────────────── */
#define TAG "RADIO"

#ifndef RIVR_SX1262_USE_DIO2_RF_SWITCH
#define RIVR_SX1262_USE_DIO2_RF_SWITCH 1
#endif

/* ── RX silence watchdog ─────────────────────────────────────────────────── */
#define RADIO_RX_SILENCE_MS  60000u
static uint32_t        s_last_rx_event_ms = 0u;
static volatile bool   s_in_rx            = false;

/* ── DIO1 ISR flag (set from ISR, cleared in main loop) ─────────────────── */
static volatile atomic_bool s_dio1_pending = ATOMIC_VAR_INIT(false);

/* no-arg trampoline for Arduino attachInterrupt (DIO1 rising edge) */
static void s_radio_isr_trampoline(void)
{
    atomic_store_explicit(&s_dio1_pending, true, memory_order_release);
}

/* Satisfies radio_sx1262.h declaration; actual ISR registration uses the
 * no-arg trampoline above via platform_dio1_attach_isr(). */
void radio_isr(void *arg)
{
    (void)arg;
    s_radio_isr_trampoline();
}

/* ── SX1262 low-level helpers ──────────────────────────────────────────────── */

static void sx_write_cmd(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint8_t buf[64];
    buf[0] = cmd;
    if (data && len) {
        memcpy(&buf[1], data, len);
    }
    uint8_t rx[64];
    platform_sx1262_wait_busy(10);
    platform_spi_transfer(buf, rx, 1u + len);
}

static void sx_read_cmd(uint8_t cmd, uint8_t *out, uint8_t out_len)
{
    uint8_t tx[65] = {cmd, 0x00};
    uint8_t rx[65];
    platform_sx1262_wait_busy(10);
    platform_spi_transfer(tx, rx, 2u + out_len);
    memcpy(out, &rx[2], out_len);
}

static void sx_write_reg(uint16_t addr, uint8_t val)
{
    uint8_t d[3] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), val };
    sx_write_cmd(0x0D, d, 3);
}

static void sx_write_buf(uint8_t offset, const uint8_t *data, uint8_t len)
{
    uint8_t hdr[2] = { 0x0E, offset };
    uint8_t rx_hdr[2];
    platform_sx1262_wait_busy(10);
    /* Keep CS low across both chunks — platform_spi_transfer would deselect
     * between them, corrupting the SX1262 WriteBuffer command. */
    platform_spi_begin();
    platform_spi_write_read_raw(hdr, rx_hdr, 2);
    platform_spi_write_read_raw(data, NULL, len);  /* MOSI only, discard MISO */
    platform_spi_end();
}

static void sx_read_buf(uint8_t offset, uint8_t *data, uint8_t len)
{
    uint8_t hdr[3] = { 0x1E, offset, 0x00 };
    uint8_t rxhdr[3];
    platform_sx1262_wait_busy(10);
    /* Keep CS low across both chunks — platform_spi_transfer would deselect
     * between them, corrupting the SX1262 ReadBuffer command. */
    platform_spi_begin();
    platform_spi_write_read_raw(hdr, rxhdr, 3);
    platform_spi_write_read_raw(NULL, data, len);  /* MISO only, send zeros */
    platform_spi_end();
}

static void sx_set_standby(void)
{
    uint8_t d = 0x00;
    sx_write_cmd(0x80, &d, 1);
    platform_sx1262_wait_busy(50);
}

/* ── Ring buffers (owned by this module; extern'd in radio_sx1262.h) ────────── */
static rf_rx_frame_t   s_rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t s_tx_storage[RF_TX_QUEUE_CAP];
rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

/* ── radio_init ───────────────────────────────────────────────────────────── */

void radio_init(void)
{
    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));

    platform_sx1262_reset();

    sx_set_standby();

    /* NOTE: Waveshare RP2040 LoRa has NO TCXO on DIO3 — do NOT call
     * SetDio3AsTcxoCtrl.  Sending it here would force the SX1262 to try to
     * power a non-existent TCXO, causing calibration to fail and producing
     * no RF output.  (MeshCore: SX126X_DIO3_TCXO_VOLTAGE=0) */

    /* SetRegulatorMode: DC-DC — must be set BEFORE Calibrate */
    { uint8_t d = 0x01; sx_write_cmd(0x96, &d, 1); }

#if RIVR_SX1262_USE_DIO2_RF_SWITCH
    /* Some SX1262 modules route the RF switch to DIO2. */
    { uint8_t d = 0x01; sx_write_cmd(0x9D, &d, 1); }
#endif

    /* Calibrate all blocks — must run AFTER SetRegulatorMode, BEFORE SetPacketType */
    { uint8_t d = 0x7F; sx_write_cmd(0x89, &d, 1); platform_sx1262_wait_busy(100); }

    /* SetPacketType = LoRa */
    { uint8_t d = 0x01; sx_write_cmd(0x8A, &d, 1); }

    /* SetRfFrequency: fRF = freq_hz * 2^25 / 32000000 (integer, exact) */
    {
        uint32_t frf = (uint32_t)(((uint64_t)RIVR_RF_FREQ_HZ << 25) / 32000000UL);
        uint8_t d[4] = {
            (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
            (uint8_t)(frf >>  8), (uint8_t)(frf)
        };
        sx_write_cmd(0x86, d, 4);
    }

    /* SetPaConfig — 0x04,0x07,0x00,0x01 = SX1262, +22 dBm max */
    { uint8_t d[4] = {0x04, 0x07, 0x00, 0x01}; sx_write_cmd(0x95, d, 4); }

    /* SetTxParams */
    { uint8_t d[2] = {(uint8_t)RF_TX_POWER_DBM, 0x04}; sx_write_cmd(0x8E, d, 2); }

    /* SetBufferBaseAddress */
    { uint8_t d[2] = {0x00, 0x00}; sx_write_cmd(0x8F, d, 2); }

    /* SetModulationParams: SF, BW, CR, LDRO */
    {
        uint8_t bw;
        switch (RF_BANDWIDTH_HZ) {
            case   7800: bw = 0x00; break;
            case  10400: bw = 0x08; break;
            case  15600: bw = 0x01; break;
            case  20800: bw = 0x09; break;
            case  31250: bw = 0x02; break;
            case  41700: bw = 0x0A; break;
            case  62500: bw = 0x03; break;
            case 125000: bw = 0x04; break;
            case 250000: bw = 0x05; break;
            case 500000: bw = 0x06; break;
            default:     bw = 0x03; break;
        }
        uint8_t ldro = (RF_SPREADING_FACTOR >= 11 && RF_BANDWIDTH_HZ <= 125000) ? 1 : 0;
        uint8_t d[4] = {(uint8_t)RF_SPREADING_FACTOR, bw, (uint8_t)RF_CODING_RATE, ldro};
        sx_write_cmd(0x8B, d, 4);
    }

    /* SetPacketParams: preamble=8, explicit header, max payload, CRC on */
    { uint8_t d[6] = {0x00, 0x08, 0x00, RF_MAX_PAYLOAD_LEN, 0x01, 0x00};
      sx_write_cmd(0x8C, d, 6); }

    /* SetDioIrqParams: enable TxDone, RxDone, Timeout on DIO1 */
    { uint8_t d[8] = {0x02, 0x03, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00};
      sx_write_cmd(0x08, d, 8); }

    /* Fix SX1262 RX gain from datasheet errata (reg 0x08AC = 0x96) */
    sx_write_reg(0x08AC, 0x96);

    /* Attach DIO1 ISR */
    platform_dio1_attach_isr(s_radio_isr_trampoline);

    RIVR_LOGI(TAG, "radio_init: SX1262 ready (%u Hz, SF%u, BW%u, CR4/%u, +%u dBm)",
        (unsigned)RIVR_RF_FREQ_HZ, (unsigned)RF_SPREADING_FACTOR,
        (unsigned)RF_BANDWIDTH_HZ, (unsigned)RF_CODING_RATE,
        (unsigned)RF_TX_POWER_DBM);
}

/* ── radio_start_rx ───────────────────────────────────────────────────────── */

void radio_start_rx(void)
{
    platform_sx1262_set_rxen(true);
    uint8_t d[3] = {0xFF, 0xFF, 0xFF};
    sx_write_cmd(0x82, d, 3);
    s_in_rx = true;
    s_last_rx_event_ms = tb_millis();
}

/* ── radio_service_rx ─────────────────────────────────────────────────────── */

void radio_service_rx(void)
{
    if (!atomic_load_explicit(&s_dio1_pending, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&s_dio1_pending, false, memory_order_release);

    uint8_t irq_bytes[2];
    sx_read_cmd(0x12, irq_bytes, 2);
    uint16_t irq = ((uint16_t)irq_bytes[0] << 8) | irq_bytes[1];

    { uint8_t d[2] = {irq_bytes[0], irq_bytes[1]}; sx_write_cmd(0x02, d, 2); }

    if (irq & 0x0002) {  /* RxDone */
        uint8_t rxstat[2];
        sx_read_cmd(0x13, rxstat, 2);
        uint8_t pkt_len    = rxstat[0];
        uint8_t buf_offset = rxstat[1];

        if (pkt_len == 0 || pkt_len > RF_MAX_PAYLOAD_LEN) {
            goto restart_rx;
        }

        rf_rx_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.len = pkt_len;

        sx_read_buf(buf_offset, frame.data, pkt_len);

        uint8_t pstat[3];
        sx_read_cmd(0x14, pstat, 3);
        frame.rssi_dbm = (int8_t)(-(pstat[0] >> 1));
        frame.snr_db   = (int8_t)((int8_t)pstat[1] >> 2);

        rb_try_push(&rf_rx_ringbuf, &frame);
    }

    if (irq & 0x0040) {  /* Timeout — restart continuous RX */
        goto restart_rx;
    }

    return;

restart_rx:
    radio_start_rx();
}

/* ── radio_transmit ───────────────────────────────────────────────────────── */

bool radio_transmit(const rf_tx_request_t *req)
{
    if (!req || req->len == 0) {
        return false;
    }

    sx_set_standby();
    platform_sx1262_set_rxen(false);

    sx_write_buf(0, req->data, req->len);

    { uint8_t d[6] = {0x00, 0x08, 0x00, req->len, 0x01, 0x00};
      sx_write_cmd(0x8C, d, 6); }

    { uint8_t d[2] = {0xFF, 0xFF}; sx_write_cmd(0x02, d, 2); }

    { uint8_t d[3] = {0x00, 0x00, 0x00}; sx_write_cmd(0x83, d, 3); }

    /* Wait for TxDone (poll with timeout ~2 s) */
    uint32_t t0 = tb_millis();
    while (true) {
        uint8_t irq_bytes[2];
        sx_read_cmd(0x12, irq_bytes, 2);
        uint16_t irq = ((uint16_t)irq_bytes[0] << 8) | irq_bytes[1];
        if (irq & 0x0001) {  /* TxDone */
            uint8_t d[2] = {0xFF, 0xFF}; sx_write_cmd(0x02, d, 2);
            break;
        }
        if ((tb_millis() - t0) > 2000u) {
            RIVR_LOGE(TAG, "TX timeout");
            g_rivr_metrics.radio_tx_fail++;
            radio_start_rx();
            return false;
        }
        delay(1);  /* yield briefly without FreeRTOS */
    }

    radio_start_rx();
    return true;
}

/* ── radio_check_timeouts ───────────────────────────────────────────────── */

void radio_check_timeouts(void)
{
    uint32_t now = tb_millis();

    if (s_last_rx_event_ms == 0u) {
        s_last_rx_event_ms = now;
        return;
    }

    if (!s_in_rx) return;

    if ((now - s_last_rx_event_ms) >= RADIO_RX_SILENCE_MS) {
        g_rivr_metrics.radio_rx_timeout++;
        RIVR_LOGW(TAG,
            "RX silent %" PRIu32 "ms – no reset (total=%" PRIu32 ")",
            (uint32_t)(now - s_last_rx_event_ms),
            g_rivr_metrics.radio_rx_timeout);
        s_last_rx_event_ms = now;
    }
}

#endif /* RIVR_PLATFORM_RP2040 */
