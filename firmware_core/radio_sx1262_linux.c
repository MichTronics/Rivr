/**
 * @file  radio_sx1262_linux.c
 * @brief SX1262 radio driver for Raspberry Pi (Linux) — ported from
 *        radio_sx1262_nrf52.c.
 *
 * Functionally identical to radio_sx1262_nrf52.c but with platform
 * dependencies replaced:
 *
 *   taskENTER_CRITICAL / taskEXIT_CRITICAL → pthread_mutex_lock/unlock
 *   vTaskDelay(pdMS_TO_TICKS(1))           → nanosleep(1 ms)
 *   platform_dio1_attach_isr()             → same API, starts pthread internally
 *   millis()                               → tb_millis()
 *
 * All SX1262 register-access logic is copied verbatim from
 * radio_sx1262_nrf52.c and in turn from radio_sx1262.c.
 *
 * Only compiled when RIVR_PLATFORM_LINUX is defined (set by Makefile.linux).
 */

#if defined(RIVR_PLATFORM_LINUX)

#include "radio_sx1262.h"
#include "platform_linux.h"
#include "timebase.h"
#include "rivr_metrics.h"
#include "rivr_log.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

/* ── Critical section ────────────────────────────────────────────────────── *
 * The DIO1 interrupt thread sets s_dio1_pending (atomic) and the main loop  *
 * reads it, so no heavy mutex is needed in the hot path.  We keep a mutex   *
 * only around the SPI bus to prevent concurrent access if a future version  *
 * adds more threads.                                                         */
static pthread_mutex_t s_spi_mutex = PTHREAD_MUTEX_INITIALIZER;

#define RADIO_ENTER_CRITICAL()  pthread_mutex_lock(&s_spi_mutex)
#define RADIO_EXIT_CRITICAL()   pthread_mutex_unlock(&s_spi_mutex)

#define TAG "RADIO"

/* ── RX silence watchdog ──────────────────────────────────────────────────── */
#define RADIO_RX_SILENCE_MS  60000u
static uint32_t        s_last_rx_event_ms = 0u;
static volatile bool   s_in_rx            = false;

/* ── DIO1 ISR flag (set by DIO1 thread, cleared in main loop) ────────────── */
static volatile atomic_bool s_dio1_pending = ATOMIC_VAR_INIT(false);

/* No-arg trampoline registered with platform_dio1_attach_isr(). */
static void s_radio_isr_trampoline(void)
{
    atomic_store_explicit(&s_dio1_pending, true, memory_order_release);
}

/* Satisfies radio_sx1262.h declaration (called from platform_linux.c ISR thread). */
void radio_isr(void *arg)
{
    (void)arg;
    s_radio_isr_trampoline();
}

/* ── 1-ms delay helper ───────────────────────────────────────────────────── */
static void delay_ms(uint32_t ms)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000L),
    };
    nanosleep(&ts, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SX1262 low-level helpers (identical to radio_sx1262_nrf52.c)
 * ══════════════════════════════════════════════════════════════════════════ */

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
    /* WriteBuffer (0x0E) + offset + payload must be one unbroken SPI
     * transaction — CE0 must stay asserted for the full burst.
     * Pack everything into a single buffer and send in one ioctl call. */
    uint8_t buf[2 + 255];
    uint8_t rx [2 + 255];
    buf[0] = 0x0E;
    buf[1] = offset;
    memcpy(&buf[2], data, len);
    platform_sx1262_wait_busy(10);
    platform_spi_transfer(buf, rx, (uint16_t)(2u + len));
}

static void sx_read_buf(uint8_t offset, uint8_t *data, uint8_t len)
{
    /* ReadBuffer (0x1E) + offset + NOP + rx bytes, all in one transaction. */
    uint8_t tx[3 + 255];
    uint8_t rx[3 + 255];
    memset(tx, 0x00, 3u + len);
    tx[0] = 0x1E;
    tx[1] = offset;
    tx[2] = 0x00;  /* NOP status byte */
    platform_sx1262_wait_busy(10);
    platform_spi_transfer(tx, rx, (uint16_t)(3u + len));
    memcpy(data, &rx[3], len);
}

static void sx_set_standby(void)
{
    uint8_t d = 0x00;
    sx_write_cmd(0x80, &d, 1);
    platform_sx1262_wait_busy(50);
}

/* ── Ring buffers (owned by this module; extern'd in radio_sx1262.h) ───────── */
static rf_rx_frame_t   s_rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t s_tx_storage[RF_TX_QUEUE_CAP];
rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

/* ══════════════════════════════════════════════════════════════════════════
 * radio_init
 * ══════════════════════════════════════════════════════════════════════════ */

void radio_init(void)
{
    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));

    platform_sx1262_reset();
    platform_sx1262_wait_busy(100);

    /* Step 1: Standby (STDBY_RC) — must be first after reset */
    sx_set_standby();

    /* Step 2: SetDio3AsTcxoCtrl — MUST be before Calibrate so the SX1262
     * powers the TCXO and waits for it to settle before calibrating.
     * Voltage=1.8V (0x02), delay=5ms (0x000140 × 15.625µs). */
    { uint8_t d[4] = {0x02, 0x00, 0x01, 0x40}; sx_write_cmd(0x97, d, 4); }

    /* Step 3: SetRegulatorMode(DC-DC=1) — MUST be before Calibrate
     * so calibration runs in the final power-supply configuration. */
    { uint8_t d = 0x01; sx_write_cmd(0x96, &d, 1); }

    /* Step 4: Calibrate(0xFF) — full calibration; TCXO must be running.
     * BUSY stays high for ~3.5 ms (calibration) + TCXO settle (5 ms). */
    { uint8_t d = 0xFF; sx_write_cmd(0x89, &d, 1); platform_sx1262_wait_busy(100); }

    /* Step 5: Standby again — chip returns to STDBY_RC after calibration */
    sx_set_standby();

    /* Step 6: SetDio2AsRfSwitchCtrl(1) — DIO2 drives the SX1262-internal
     * antenna switch automatically (high during TX, low during RX/STDBY).
     * This does NOT conflict with the external RXEN/TXEN GPIOs, which
     * control the E22-900M30S external PA/LNA. Both can be active at once. */
    { uint8_t d = 0x01; sx_write_cmd(0x9D, &d, 1); }

    /* Step 7: SetPacketType = LoRa */
    { uint8_t d = 0x01; sx_write_cmd(0x8A, &d, 1); }

    /* Step 8: SetRfFrequency */
    {
        uint32_t frf = (uint32_t)(((uint64_t)RIVR_RF_FREQ_HZ << 25) / 32000000UL);
        uint8_t d[4] = {
            (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
            (uint8_t)(frf >>  8), (uint8_t)(frf)
        };
        sx_write_cmd(0x86, d, 4);
    }

    /* Step 9: SetPaConfig — SX1262 HP-PA, +22 dBm max output on chip.
     * paDutyCycle=0x04, hpMax=0x07, deviceSel=0x00 (SX1262), paLut=0x01. */
    { uint8_t d[4] = {0x04, 0x07, 0x00, 0x01}; sx_write_cmd(0x95, d, 4); }

    /* Step 10: SetModulationParams: SF, BW, CR, LDRO */
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
        /* RF_CODING_RATE is the CR denominator (5..8).  SX1262 SetModulationParams
         * uses a register encoding: CR4/5=0x01, CR4/6=0x02, CR4/7=0x03, CR4/8=0x04.
         * Passing the raw denominator (e.g. 8) is wrong — the ESP32 driver maps
         * this at compile time via _RF_CR_SX1262_REG; replicate that here. */
        uint8_t cr_reg;
        switch (RF_CODING_RATE) {
            case 5:  cr_reg = 0x01u; break;
            case 6:  cr_reg = 0x02u; break;
            case 7:  cr_reg = 0x03u; break;
            case 8:  cr_reg = 0x04u; break;
            default: cr_reg = 0x01u; break;  /* fallback: CR4/5 */
        }
        uint8_t d[4] = {(uint8_t)RF_SPREADING_FACTOR, bw, cr_reg, ldro};
        sx_write_cmd(0x8B, d, 4);
    }

    /* Step 11: SetPacketParams: preamble=8, explicit header, max payload, CRC on */
    { uint8_t d[6] = {0x00, 0x08, 0x00, RF_MAX_PAYLOAD_LEN, 0x01, 0x00};
      sx_write_cmd(0x8C, d, 6); }

    /* Step 12: SetTxParams: power, rampTime=40µs */
    { uint8_t d[2] = {(uint8_t)RF_TX_POWER_DBM, 0x04}; sx_write_cmd(0x8E, d, 2); }

    /* Step 13: SetBufferBaseAddress */
    { uint8_t d[2] = {0x00, 0x00}; sx_write_cmd(0x8F, d, 2); }

    /* Step 14: SetDioIrqParams: TxDone|RxDone|Timeout on DIO1 */
    { uint8_t d[8] = {0x02, 0x03, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00};
      sx_write_cmd(0x08, d, 8); }

    /* Step 15: Boosted RX gain (reg 0x08AC = 0x96, datasheet errata) */
    sx_write_reg(0x08AC, 0x96);

    /* SX1262 errata §15.1: receiver spurious reception fix.
     * For all BW != 500 kHz, bit 2 of reg 0x08B5 must be SET.
     * (RadioLib fixSensitivity(): sensitivityConfig |= 0x04) */
    {
        uint8_t tx[5] = { 0x1D, 0x08, 0xB5, 0x00, 0x00 };  /* 0x1D = ReadRegister */
        uint8_t rx[5] = {0};
        platform_sx1262_wait_busy(10);
        platform_spi_transfer(tx, rx, 5);
        uint8_t reg = rx[4];
        reg |= 0x04u;
        sx_write_reg(0x08B5, reg);
    }

    /* SX1262 errata §15.2: PA clamp fix — overly eager PA clamping
     * during initial NFET turn-on causes weak / failed TX without this.
     * (RadioLib fixPaClamping(): clampConfig |= 0x1E on reg 0x08D8) */
    {
        uint8_t tx[5] = { 0x1D, 0x08, 0xD8, 0x00, 0x00 };  /* 0x1D = ReadRegister */
        uint8_t rx[5] = {0};
        platform_sx1262_wait_busy(10);
        platform_spi_transfer(tx, rx, 5);
        uint8_t reg = rx[4];
        reg |= 0x1Eu;
        sx_write_reg(0x08D8, reg);
    }

    /* Diagnostic: read GetStatus to confirm SPI MISO is working.
     * Chip mode field is bits [5:3]; 0x44 = STDBY_RC, 0x2C = STDBY_XOSC.
     * 0x00 means MISO is floating or grounded — check wiring. */
    {
        uint8_t st[1] = {0};
        sx_read_cmd(0xC0, st, 1);
        RIVR_LOGI(TAG, "radio_init: GetStatus=0x%02X (0x44=STDBY_RC ok, 0x00=MISO broken)", st[0]);
    }

    /* Start the DIO1 interrupt monitoring thread. */
    platform_dio1_attach_isr(s_radio_isr_trampoline);

    RIVR_LOGI(TAG, "radio_init: SX1262 ready (%u Hz, SF%u, BW%u, CR4/%u, +%u dBm)",
        (unsigned)RIVR_RF_FREQ_HZ, (unsigned)RF_SPREADING_FACTOR,
        (unsigned)RF_BANDWIDTH_HZ, (unsigned)RF_CODING_RATE,
        (unsigned)RF_TX_POWER_DBM);
}

/* ══════════════════════════════════════════════════════════════════════════
 * radio_start_rx
 * ══════════════════════════════════════════════════════════════════════════ */

void radio_start_rx(void)
{
    platform_sx1262_set_rxen(true);
    /* SetRx: timeout = 0xFFFFFF = continuous */
    uint8_t d[3] = {0xFF, 0xFF, 0xFF};
    sx_write_cmd(0x82, d, 3);
    s_in_rx = true;
    s_last_rx_event_ms = tb_millis();
}

/* ══════════════════════════════════════════════════════════════════════════
 * radio_service_rx
 * ══════════════════════════════════════════════════════════════════════════ */

void radio_service_rx(void)
{
    if (!atomic_load_explicit(&s_dio1_pending, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&s_dio1_pending, false, memory_order_release);

    RADIO_ENTER_CRITICAL();

    /* GetIrqStatus */
    uint8_t irq_bytes[2];
    sx_read_cmd(0x12, irq_bytes, 2);
    uint16_t irq = ((uint16_t)irq_bytes[0] << 8) | irq_bytes[1];

    /* ClearIrqStatus */
    { uint8_t d[2] = {irq_bytes[0], irq_bytes[1]}; sx_write_cmd(0x02, d, 2); }

    if (irq & 0x0002) {  /* RxDone */
        /* GetRxBufferStatus */
        uint8_t rxstat[2];
        sx_read_cmd(0x13, rxstat, 2);
        uint8_t pkt_len    = rxstat[0];
        uint8_t buf_offset = rxstat[1];

        /* pkt_len is uint8_t and RF_MAX_PAYLOAD_LEN == 255, so the upper
         * bound is guaranteed by the type; only guard against zero-length. */
        if (pkt_len == 0) {
            goto restart_rx;
        }

        rf_rx_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.len = pkt_len;

        sx_read_buf(buf_offset, frame.data, pkt_len);

        /* GetPacketStatus: rssi, snr, signal_rssi */
        uint8_t pstat[3];
        sx_read_cmd(0x14, pstat, 3);
        frame.rssi_dbm   = (int8_t)(-(pstat[0] >> 1));
        frame.snr_db     = (int8_t)((int8_t)pstat[1] >> 2);
        frame.rx_mono_ms = tb_millis();

        rb_try_push(&rf_rx_ringbuf, &frame);
        s_last_rx_event_ms = tb_millis();
    }

    if (irq & 0x0040) {  /* Timeout — restart continuous RX */
        goto restart_rx;
    }

    RADIO_EXIT_CRITICAL();
    return;

restart_rx:
    RADIO_EXIT_CRITICAL();
    radio_start_rx();
}

/* ══════════════════════════════════════════════════════════════════════════
 * radio_transmit
 * ══════════════════════════════════════════════════════════════════════════ */

bool radio_transmit(const rf_tx_request_t *req)
{
    if (!req || req->len == 0) {
        return false;
    }

    RADIO_ENTER_CRITICAL();

    sx_set_standby();
    platform_sx1262_set_rxen(false);

    sx_write_buf(0, req->data, req->len);

    /* SetPacketParams — update payload length */
    { uint8_t d[6] = {0x00, 0x08, 0x00, req->len, 0x01, 0x00};
      sx_write_cmd(0x8C, d, 6); }

    /* ClearIrqStatus */
    { uint8_t d[2] = {0xFF, 0xFF}; sx_write_cmd(0x02, d, 2); }

    /* SetTx: no timeout (use airtime estimate in main loop) */
    { uint8_t d[3] = {0x00, 0x00, 0x00}; sx_write_cmd(0x83, d, 3); }

    /* Poll for TxDone (timeout ~2 s) */
    uint32_t t0 = tb_millis();
    while (true) {
        uint8_t irq_bytes[2];
        sx_read_cmd(0x12, irq_bytes, 2);
        uint16_t irq = ((uint16_t)irq_bytes[0] << 8) | irq_bytes[1];
        if (irq & 0x0001) {  /* TxDone */
            uint8_t d[2] = {0xFF, 0xFF};
            sx_write_cmd(0x02, d, 2);
            break;
        }
        if ((tb_millis() - t0) > 2000u) {
            uint8_t st[1] = {0};
            sx_read_cmd(0xC0, st, 1);
            RIVR_LOGE(TAG, "TX timeout — irq=0x%02X%02X GetStatus=0x%02X "
                      "(0x00=MISO broken, 0xD2=TX state ok)",
                      irq_bytes[0], irq_bytes[1], st[0]);
            g_rivr_metrics.radio_tx_fail++;
            RADIO_EXIT_CRITICAL();
            radio_start_rx();
            return false;
        }
        RADIO_EXIT_CRITICAL();
        delay_ms(1);
        RADIO_ENTER_CRITICAL();
    }

    RADIO_EXIT_CRITICAL();
    radio_start_rx();
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * radio_check_timeouts
 * ══════════════════════════════════════════════════════════════════════════ */

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

#endif /* RIVR_PLATFORM_LINUX */
