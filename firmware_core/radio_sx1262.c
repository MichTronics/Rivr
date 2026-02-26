/**
 * @file  radio_sx1262.c
 * @brief SX1262 LoRa radio driver.
 *
 * DETERMINISM INVARIANTS
 * ──────────────────────
 *  • radio_isr() writes ONLY to rf_rx_ringbuf (SPSC push).
 *  • radio_isr() never calls malloc, printf, or any RIVR function.
 *  • radio_transmit() is called only from the main loop.
 *  • All SX1262 register accesses go through platform_spi_transfer().
 *
 * SX1262 COMMAND REFERENCE (SX1261/62 UM v2.1 + RadioLib SX126x_commands.h)
 * ────────────────────────────────────────────────────────────────────────────
 *  0x80  SetStandby(0=STDBY_RC, 1=STDBY_XOSC)
 *  0x82  SetRx(timeout[3])
 *  0x83  SetTx(timeout[3])
 *  0x84  SetSleep(sleepConfig)
 *  0x86  SetRfFrequency(frf[4])
 *  0x08  SetDioIrqParams(irqMask[2], dio1[2], dio2[2], dio3[2])
 *  0x89  Calibrate(calibParam)
 *  0x8A  SetPacketType(0=GFSK, 1=LoRa)        ← NOT 0x8F
 *  0x8B  SetModulationParams(SF, BW, CR, LDRO)
 *  0x8C  SetPacketParams(preamble[2],headerType,payloadLen,crc,invertIQ)
 *  0x8E  SetTxParams(power, rampTime)
 *  0x8F  SetBufferBaseAddress(txBase, rxBase)  ← NOT SetPacketType
 *  0x95  SetPaConfig(paDutyCycle, hpMax, deviceSel, paLut)
 *  0x96  SetRegulatorMode(0=LDO, 1=DC-DC)
 *  0x97  SetDio3AsTcxoCtrl(voltage, delay[3])
 *  0x9D  SetDio2AsRfSwitchCtrl(enable)
 *  0x0E  WriteBuffer(offset, data...)
 *  0x1E  ReadBuffer(offset, NOP, data...)
 *  0x12  GetIrqStatus() → NOP + 2 bytes
 *  0x13  GetRxBufferStatus() → NOP + 2 bytes (payloadLen, startAddr)
 *  0x14  GetPacketStatus() → NOP + 3 bytes
 *  0x02  ClearIrqStatus(mask[2])
 *
 * LoRa BW encoding (SetModulationParams param2, Table 13-47):
 *   0x00=7.8  0x08=10.4  0x01=15.6  0x09=20.8  0x02=31.25
 *   0x0A=41.7 0x03=62.5  0x04=125   0x05=250   0x06=500  kHz
 */

#include "radio_sx1262.h"
#include "platform_esp32.h"
#include "timebase.h"
#include "driver/gpio.h"    /* gpio_isr_handler_add */
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "RADIO"

/* ── Ring-buffer storage (static, no heap) ───────────────────────────────── */
static rf_rx_frame_t  s_rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t s_tx_storage[RF_TX_QUEUE_CAP];

/** True while the radio is in continuous-RX mode (cleared during TX). */
static volatile bool s_in_rx = false;

/** Set by radio_isr() when DIO1 fires; cleared by radio_service_rx(). */
static volatile bool s_dio1_pending = false;

rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

void radio_init_buffers_only(void)
{
    /* Initialise ring-buffers without any SPI/GPIO access.
     * Safe to call before platform_init() in simulation builds. */
    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));
    ESP_LOGI(TAG, "radio_init_buffers_only: ringbufs ready (SIM MODE)");
}

/* ── Internal SX1262 register helpers ───────────────────────────────────── */

static void sx1262_cmd(uint8_t cmd, const uint8_t *params, uint8_t n_params)
{
    uint8_t tx_buf[32];
    uint8_t rx_buf[32];
    tx_buf[0] = cmd;
    if (n_params > 0 && params) {
        memcpy(tx_buf + 1, params, n_params);
    }
    platform_spi_transfer(tx_buf, rx_buf, 1 + n_params);
}

static void sx1262_read_cmd(uint8_t cmd, uint8_t *out, uint8_t n_out)
{
    uint8_t tx_buf[34] = { cmd, 0x00 };  /* cmd + NOP status byte */
    uint8_t rx_buf[34] = {0};
    platform_spi_transfer(tx_buf, rx_buf, 2 + n_out);
    memcpy(out, rx_buf + 2, n_out);
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

void radio_init(void)
{
    ESP_LOGI(TAG, "radio_init: resetting SX1262");

    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));

    platform_sx1262_reset();
    platform_sx1262_wait_busy(100);

    /* SetStandby(STDBY_RC) — must be first command after reset */
    uint8_t p = 0x00;
    sx1262_cmd(0x80, &p, 1);   /* 0x80 = SetStandby */
    platform_sx1262_wait_busy(10);

    /* SetDio3AsTcxoCtrl(voltage=1.8V, delay=5ms)
     * The E22-900M30S powers its TCXO from SX1262 DIO3.
     * Without this the oscillator never starts → no RF output even though
     * SPI responds normally.
     * Voltage byte 0x02 = 1.8 V; delay = 5 ms / 15.625 µs = 320 = 0x000140. */
    {
        uint8_t tcxo[4] = { 0x02, 0x00, 0x01, 0x40 };
        sx1262_cmd(0x97, tcxo, 4);
        platform_sx1262_wait_busy(10);
    }

    /* SetRegulatorMode(DC-DC=1): the E22-900M30S uses a DC-DC converter.
     * Without this the chip runs in LDO mode which may not supply enough
     * current for 30 dBm output. Must be set before Calibrate. */
    p = 0x01;
    sx1262_cmd(0x96, &p, 1);   /* 0x96 = SetRegulatorMode */
    platform_sx1262_wait_busy(10);

    /* Calibrate(0xFF) — run full calibration with TCXO powered.
     * Required after SetDio3AsTcxoCtrl; takes up to 3.5 ms. */
    p = 0xFF;
    sx1262_cmd(0x89, &p, 1);   /* 0x89 = Calibrate */
    platform_sx1262_wait_busy(50);   /* generous budget for full cal */

    /* SetStandby(STDBY_RC) again — chip exits calibration in standby */
    p = 0x00;
    sx1262_cmd(0x80, &p, 1);   /* 0x80 = SetStandby */
    platform_sx1262_wait_busy(10);

    /* SetDio2AsRfSwitchCtrl(1): DIO2 also drives TXEN automatically during TX.
     * Combined with the explicit GPIO13 drive in platform_sx1262_set_rxen()
     * this covers both board variants (DIO2→TXEN traced or not). */
    p = 0x01;
    sx1262_cmd(0x9D, &p, 1);
    platform_sx1262_wait_busy(10);

    /* SetPacketType(LoRa=1)
     * 0x8A = SetPacketType  (0x8F = SetBufferBaseAddress — not this!) */
    p = 0x01;
    sx1262_cmd(0x8A, &p, 1);   /* 0x8A = SetPacketType */
    platform_sx1262_wait_busy(10);

    /* SetRfFrequency: fRF = freq_hz * 2^25 / 32000000 (integer, no float rounding error).
     * Float approximation (/ 0.95367f) introduced ~17 kHz error at 869 MHz. */
    uint32_t frf = (uint32_t)(((uint64_t)RF_FREQ_HZ << 25) / 32000000UL);
    uint8_t freq_params[4] = {
        (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
        (uint8_t)(frf >>  8), (uint8_t)(frf)
    };
    sx1262_cmd(0x86, freq_params, 4);
    platform_sx1262_wait_busy(10);

    /* SetPaConfig: required for E22-900M30S high-power (HP) PA.
     * paDutyCycle=0x04, hpMax=0x07, deviceSel=0x00 (SX1262), paLut=0x01.
     * Without this the PA is unconfigured and produces no output. */
    {
        uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };
        sx1262_cmd(0x95, pa, 4);
        platform_sx1262_wait_busy(10);
    }

    /* SetModulationParams: SF=8, BW=0x04(125kHz), CR=0x04(4/8), LDRO=0
     * Authoritative BW encoding (RadioLib / SX1262 UM v2.1 Table 13-47):
     *   0x00=7.8   0x08=10.4  0x01=15.6  0x09=20.8  0x02=31.25
     *   0x0A=41.7  0x03=62.5  0x04=125   0x05=250   0x06=500  kHz */
    uint8_t mod_params[4] = { RF_SPREADING_FACTOR, 0x04, 0x04, 0x00 };
    sx1262_cmd(0x8B, mod_params, 4);   /* 0x8B = SetModulationParams */
    platform_sx1262_wait_busy(10);

    /* SetPacketParams: preamble=8, explicit header, maxPayload, CRC=on, noInvertIQ */
    uint8_t pkt_params[6] = {
        0x00, RF_PREAMBLE_LEN,  /* preamble MSB, LSB */
        0x00,                   /* variable length header */
        RF_MAX_PAYLOAD_LEN,     /* maxPayloadLength */
        0x01,                   /* CRC on */
        0x00                    /* IQ standard */
    };
    sx1262_cmd(0x8C, pkt_params, 6);   /* 0x8C = SetPacketParams */
    platform_sx1262_wait_busy(10);

    /* SetTxParams: +22 dBm (SX1262 max), rampTime=40µs.
     * The E22-900M30S external PA boosts this to ~30 dBm. */
    uint8_t tx_params[2] = { 0x16, 0x04 };
    sx1262_cmd(0x8E, tx_params, 2);   /* 0x8E = SetTxParams */
    platform_sx1262_wait_busy(10);

    /* SetDioIrqParams: enable TxDone+RxDone+Timeout on DIO1 */
    uint8_t irq_params[8] = {
        0x02, 0x03,   /* irqMask: TxDone(0x0001) | RxDone(0x0002) */
        0x02, 0x03,   /* DIO1 */
        0x00, 0x00,   /* DIO2 */
        0x00, 0x00    /* DIO3 */
    };
    sx1262_cmd(0x08, irq_params, 8);   /* 0x08 = SetDioIrqParams */
    platform_sx1262_wait_busy(10);

    /* Attach DIO1 ISR */
    gpio_isr_handler_add(PIN_SX1262_DIO1, radio_isr, NULL);

    ESP_LOGI(TAG, "radio_init: done (SF%u BW%ukHz @ %luHz)",
             RF_SPREADING_FACTOR, RF_BANDWIDTH_KHZ, (unsigned long)RF_FREQ_HZ);
}

void radio_start_rx(void)
{
    /* Enable receive path on the RF switch */
    platform_sx1262_set_rxen(true);

    /* SetRx with timeout=0xFFFFFF (continuous) */
    uint8_t timeout[3] = { 0xFF, 0xFF, 0xFF };
    sx1262_cmd(0x82, timeout, 3);   /* 0x82 = SetRx */
    platform_sx1262_wait_busy(20);  /* 20 ms: covers post-TX PA deassert settle
                                     * (5 ms was too tight and triggered spurious
                                     * BUSY timeout logs after every TX) */
    s_in_rx = true;
    ESP_LOGI(TAG, "RX mode started");
}

/* ── ISR ─────────────────────────────────────────────────────────────────── *
 *
 * BOUNDED-TIME PATH: no calls to RIVR, no allocation, no printf.
 *
 * Total worst-case time at 8 MHz SPI:
 *   GetIrqStatus (3 SPI bytes)          ≈   3 µs
 *   ClearIrqStatus (3 SPI bytes)        ≈   3 µs
 *   GetRxBufferStatus (4 SPI bytes)     ≈   4 µs
 *   ReadBuffer (3 + 255 SPI bytes max)  ≈ 260 µs
 *   rb_try_push (memcpy 258 bytes)      ≈  10 µs
 *   TOTAL                               < 290 µs
 * ─────────────────────────────────────────────────────────────────────────── */
/* ISR: ONLY sets s_dio1_pending.  No SPI calls allowed from ISR context —
 * spi_device_transmit() takes a FreeRTOS semaphore which deadlocks the
 * scheduler and triggers the Interrupt WDT.  All SPI work is deferred to
 * radio_service_rx() which is called from the main-loop task. */
void IRAM_ATTR radio_isr(void *arg)
{
    (void)arg;
    s_dio1_pending = true;
}

/* Called from the main loop after every rivr_tick().  Drains all pending
 * DIO1 events (there is normally at most one per loop iteration). */
void radio_service_rx(void)
{
    if (!s_dio1_pending) return;
    s_dio1_pending = false;

    /* 1. Read IRQ status */
    uint8_t irq_status[2] = {0};
    sx1262_read_cmd(0x12, irq_status, 2);   /* 0x12 = GetIrqStatus */
    uint16_t irq = ((uint16_t)irq_status[0] << 8) | irq_status[1];

    /* 2. Clear all IRQ flags */
    uint8_t clr[2] = { irq_status[0], irq_status[1] };
    sx1262_cmd(0x02, clr, 2);   /* 0x02 = ClearIrqStatus */

    if (!(irq & 0x0002)) return;  /* Not RxDone – ignore (TxDone, Timeout, etc.) */

    /* 3. GetRxBufferStatus → payloadLen, startAddr */
    uint8_t buf_status[2] = {0};
    sx1262_read_cmd(0x13, buf_status, 2);   /* 0x13 = GetRxBufferStatus */
    uint8_t payload_len  = buf_status[0];
    uint8_t start_addr   = buf_status[1];

    if (payload_len == 0 || payload_len > RF_MAX_PAYLOAD_LEN) return;

    /* 4. GetPacketStatus (0x14): RssiPkt, SnrPkt, SignalRssiPkt */
    int16_t pkt_rssi_dbm = -99;
    int8_t  pkt_snr_db   = 0;
    {
        uint8_t pkt_status[3] = {0};
        sx1262_read_cmd(0x14, pkt_status, 3);
        pkt_rssi_dbm = -(int16_t)pkt_status[0] / 2;
        pkt_snr_db   = (int8_t)pkt_status[1] / 4;
    }

    /* 5. ReadBuffer: command=0x1E, offset=start_addr, NOP, then payload */
    rf_rx_frame_t frame;
    memset(&frame, 0, sizeof(frame));   /* from_id must be 0 on real hardware;
                                         * the SX1262 has no notion of sender ID.
                                         * Garbage here would poison route_cache. */
    {
        uint8_t tx_buf[4] = { 0x1E, start_addr, 0x00, 0x00 };   /* 0x1E = ReadBuffer */
        uint8_t rx_buf[4 + RF_MAX_PAYLOAD_LEN];
        memset(rx_buf, 0, sizeof(rx_buf));
        platform_spi_transfer(tx_buf, rx_buf, 4 + payload_len);
        memcpy(frame.data, rx_buf + 3, payload_len);
    }
    frame.len         = payload_len;
    frame.rx_mono_ms  = (uint32_t)atomic_load_explicit(&g_mono_ms, memory_order_relaxed);
    frame.rssi_dbm    = pkt_rssi_dbm;
    frame.snr_db      = pkt_snr_db;

    /* Push into ringbuf (may silently drop if full) */
    rb_try_push(&rf_rx_ringbuf, &frame);
}

/* ── TX ──────────────────────────────────────────────────────────────────── */

bool radio_transmit(const rf_tx_request_t *req)
{
    if (!req || req->len == 0) return false;

    platform_sx1262_wait_busy(10);

    /* WriteBuffer: 0x0E + offset=0 + payload
     * NOTE: WriteBuffer takes [opcode][offset][data…] with NO NOP byte
     * between offset and data — unlike ReadBuffer which needs one.
     * A spurious 0x00 here would prepend a null byte to every TX frame,
     * corrupting byte[0] of the RIVR magic and causing all receivers to
     * reject the frame as "bad magic (foreign device?)". */
    uint8_t tx_cmd[2 + RF_MAX_PAYLOAD_LEN];
    tx_cmd[0] = 0x0E;   /* WriteBuffer */
    tx_cmd[1] = 0x00;   /* offset into SX1262 TX ring buffer */
    memcpy(tx_cmd + 2, req->data, req->len);
    platform_spi_transfer(tx_cmd, NULL, 2 + req->len);
    platform_sx1262_wait_busy(5);

    /* SetStandby before TX — chip may still be in RX mode */
    {
        uint8_t stdby = 0x00;
        sx1262_cmd(0x80, &stdby, 1);   /* 0x80 = SetStandby(RC) */
        platform_sx1262_wait_busy(5);
    }

    /* Update payload length in packet params */
    uint8_t pkt_params[6] = {
        0x00, RF_PREAMBLE_LEN, 0x00, req->len, 0x01, 0x00
    };
    sx1262_cmd(0x8C, pkt_params, 6);   /* 0x8C = SetPacketParams */
    platform_sx1262_wait_busy(5);

    /* Switch antenna to TX path (RXEN low, TXEN high) */
    s_in_rx = false;
    platform_sx1262_set_rxen(false);

    /* SetTx with timeout = ToA × 2 converted to SX1262 ticks.
     * SX1262 timer resolution = 15.625 µs/tick → N = toa_us × 2 / 15.625
     *   = toa_us × 2 × 64 / 1000 = toa_us × 128 / 1000 = toa_us × 16 / 125
     * Using 2× (not 1.5×) gives 73 ms extra headroom on a 156 ms frame
     * without risking a premature hardware-timeout interrupt.               */
    uint32_t timeout_ticks = (req->toa_us * 16u) / 125u;
    uint8_t tx_timeout[3] = {
        (uint8_t)(timeout_ticks >> 16),
        (uint8_t)(timeout_ticks >>  8),
        (uint8_t)(timeout_ticks)
    };
    sx1262_cmd(0x83, tx_timeout, 3);   /* 0x83 = SetTx  ← was 0x82=SetRx, the root cause */

    /* Poll for TxDone (up to toa_us × 2 ms) */
    uint32_t t0 = tb_millis();
    uint32_t deadline_ms = t0 + req->toa_us / 1000u * 2u + 100u;

    while (tb_millis() < deadline_ms) {
        uint8_t irq[2] = {0};
        sx1262_read_cmd(0x12, irq, 2);   /* 0x12 = GetIrqStatus */
        uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];
        if (flags & 0x0001) {   /* TxDone */
            uint8_t clr[2] = { irq[0], irq[1] };
            sx1262_cmd(0x02, clr, 2);   /* 0x02 = ClearIrqStatus */
            radio_start_rx();   /* return to RX */
            return true;
        }
        if (flags & 0x0200) {   /* Timeout */
            ESP_LOGE(TAG, "TX timeout");
            radio_start_rx();
            return false;
        }
    }
    ESP_LOGE(TAG, "TX deadline exceeded");
    radio_start_rx();
    return false;
}

/* ── Frame decoder ───────────────────────────────────────────────────────── */

uint8_t radio_decode_frame(const rf_rx_frame_t *frame, char *out_buf, uint8_t out_len)
{
    if (!frame || frame->len < 3 || out_len == 0) return 0;

    const char *prefix = "DATA";
    switch (frame->data[0]) {
        case RF_FRAME_CHAT:   prefix = "CHAT";   break;
        case RF_FRAME_BEACON: prefix = "BEACON"; break;
        case RF_FRAME_ACK:    prefix = "ACK";    break;
        default:              prefix = "DATA";   break;
    }

    /* Payload starts at byte 3 (after [type, tick_lo, tick_hi]) */
    uint8_t payload_len = frame->len - 3u;
    int n = snprintf(out_buf, out_len, "%s:", prefix);
    if (n < 0 || n >= out_len) return 0;

    uint8_t remain = out_len - (uint8_t)n - 1u;
    uint8_t copy   = payload_len < remain ? payload_len : remain;
    memcpy(out_buf + n, frame->data + 3, copy);
    out_buf[n + copy] = '\0';
    return (uint8_t)(n + copy);
}

uint16_t radio_frame_sender_tick(const rf_rx_frame_t *frame)
{
    if (!frame || frame->len < 3) return 0;
    return (uint16_t)frame->data[1] | ((uint16_t)frame->data[2] << 8);
}

void radio_poll_rx(void)
{
    /* Used in polling mode (no ISR).  In ISR mode, this is a no-op. */
}

int16_t radio_get_rssi_inst(void)
{
    /* GetRssiInst (0x15): valid only in RX mode.
     * Returns noise floor (~-120 dBm) when called outside an RX window. */
    if (!s_in_rx) return -120;
    uint8_t raw = 0;
    sx1262_read_cmd(0x15, &raw, 1);
    return -(int16_t)raw / 2;
}
