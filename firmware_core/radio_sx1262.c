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
 *  0x0D  WriteRegister(addr[2], data...)
 *  0x1D  ReadRegister(addr[2], NOP, data...)
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
#include "rivr_metrics.h"
#include "driver/gpio.h"    /* gpio_isr_handler_add */
#include "esp_log.h"
#include "esp_task_wdt.h"  /* esp_task_wdt_reset — keeps WDT happy during long TX polls */
#include "freertos/FreeRTOS.h"  /* portMUX_TYPE, portENTER_CRITICAL* */
#include "rivr_log.h"
#include <string.h>
#include <stdio.h>

/* ── Compile-time SX1262 register encodings ─────────────────────────────── */
/* BW kHz → SetModulationParams param2 (SX1262 UM v2.1 Table 13-47)        */
#if   RF_BANDWIDTH_KHZ == 7
#  define _RF_BW_SX1262_REG  0x00u
#elif RF_BANDWIDTH_KHZ == 10
#  define _RF_BW_SX1262_REG  0x08u
#elif RF_BANDWIDTH_KHZ == 15
#  define _RF_BW_SX1262_REG  0x01u
#elif RF_BANDWIDTH_KHZ == 20
#  define _RF_BW_SX1262_REG  0x09u
#elif RF_BANDWIDTH_KHZ == 31
#  define _RF_BW_SX1262_REG  0x02u
#elif RF_BANDWIDTH_KHZ == 41
#  define _RF_BW_SX1262_REG  0x0Au
#elif RF_BANDWIDTH_KHZ == 62
#  define _RF_BW_SX1262_REG  0x03u
#elif RF_BANDWIDTH_KHZ == 125
#  define _RF_BW_SX1262_REG  0x04u
#elif RF_BANDWIDTH_KHZ == 250
#  define _RF_BW_SX1262_REG  0x05u
#elif RF_BANDWIDTH_KHZ == 500
#  define _RF_BW_SX1262_REG  0x06u
#else
#  error "RF_BANDWIDTH_KHZ: unsupported — use 7/10/15/20/31/41/62/125/250/500"
#endif
/* CR denominator (4/N) → SetModulationParams param3                        */
#if   RF_CODING_RATE == 5
#  define _RF_CR_SX1262_REG  0x01u
#elif RF_CODING_RATE == 6
#  define _RF_CR_SX1262_REG  0x02u
#elif RF_CODING_RATE == 7
#  define _RF_CR_SX1262_REG  0x03u
#elif RF_CODING_RATE == 8
#  define _RF_CR_SX1262_REG  0x04u
#else
#  error "RF_CODING_RATE: unsupported — use 5, 6, 7, or 8 (for CR 4/5..4/8)"
#endif
#include <inttypes.h>

#define TAG "RADIO"

/* ── Ring-buffer storage (static, no heap) ───────────────────────────────── */
static rf_rx_frame_t  s_rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t s_tx_storage[RF_TX_QUEUE_CAP];

/** True while the radio is in continuous-RX mode (cleared during TX). */
static volatile bool s_in_rx = false;

/** Set by radio_isr() when DIO1 fires; cleared by radio_service_rx(). */
static volatile bool s_dio1_pending = false;

/* Spinlock protecting s_dio1_pending against the dual-core race where
 * the ISR on CPU1 sets the flag while the main loop on CPU0 is between
 * the read and the clear.  On single-core builds this degenerates to a
 * simple interrupt-disable; on dual-core it also serialises CPUs. */
static portMUX_TYPE s_dio1_mux = portMUX_INITIALIZER_UNLOCKED;

/** Consecutive TX failures; triggers radio_hard_reset() at 3. */
static uint8_t s_tx_fail_streak = 0;

/** Consecutive spurious (non-RxDone, non-TxDone) DIO1 events; reset on valid RxDone. */
static uint8_t s_spurious_irq_streak = 0;

/* ── Recovery / backoff state ────────────────────────────────────────────── */

/** Minimum milliseconds between consecutive hard resets (backoff guard).
 * Enforces at most one hard reset per RADIO_RESET_BACKOFF_MS — prevents
 * reset storms under persistent faults (BUSY always stuck, TX always failing).
 * 5 000 ms → at most 12 resets per minute while still recovering promptly. */
#define RADIO_RESET_BACKOFF_MS     5000u
/** Consecutive BUSY-stuck stalls in radio_transmit() before forced reset. */
#define RADIO_BUSY_STUCK_MAX          3u
/** RX radio silence threshold: no DIO1 event for this long → metric + WARN log. */
#define RADIO_RX_SILENCE_MS       60000u

/** True once radio_guard_reset() has fired at least once. */
static bool     s_reset_happened    = false;
/** tb_millis() at the most recent radio_guard_reset() fire. */
static uint32_t s_last_reset_ms     = 0u;
/** tb_millis() of the most recent DIO1 event (any reason). */
static uint32_t s_last_rx_event_ms  = 0u;
/** Consecutive BUSY-stuck stall count; reset to 0 on any successful BUSY low. */
static uint8_t  s_busy_stuck_streak  = 0u;

/* ── Fault injection (test builds: -DRIVR_FAULT_INJECT=1) ───────────────── */
#ifdef RIVR_FAULT_INJECT
bool    g_fault_busy_stuck = false;   /**< Makes wait_busy always return false   */
bool    g_fault_tx_no_done = false;   /**< Suppresses TxDone flag in TX poll     */
bool    g_fault_rx_silence = false;   /**< Suppresses DIO1 event dispatch        */
uint8_t g_fault_crc_fail   = 0u;     /**< Burst CRC-error injection counter     */

static bool _fi_wait_busy(uint32_t ms)
{
    if (g_fault_busy_stuck) return false;
    return platform_sx1262_wait_busy(ms);
}
/* Redirect all wait_busy calls inside this TU through the fault wrapper */
#define platform_sx1262_wait_busy   _fi_wait_busy
#endif /* RIVR_FAULT_INJECT */

rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

void radio_init_buffers_only(void)
{
    /* Initialise ring-buffers without any SPI/GPIO access.
     * Safe to call before platform_init() in simulation builds. */
    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));
    RIVR_LOGI(TAG, "radio_init_buffers_only: ringbufs ready (SIM MODE)");
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

/**
 * @brief Write the full SX1262 register configuration sequence.
 *
 * Assumes the chip has already been reset (RESET pulse + BUSY low).
 * Does NOT touch ring-buffers and does NOT attach the DIO1 ISR — those
 * are one-time operations done in radio_init().
 * Called by both radio_init() (first boot) and radio_hard_reset().
 */
static void radio_apply_config(void)
{
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
    {
        uint8_t mod_params[4] = { RF_SPREADING_FACTOR, _RF_BW_SX1262_REG, _RF_CR_SX1262_REG, 0x00 };
        sx1262_cmd(0x8B, mod_params, 4);   /* 0x8B = SetModulationParams */
        platform_sx1262_wait_busy(10);
    }

    /* SetPacketParams: preamble=8, explicit header, maxPayload, CRC=on, noInvertIQ */
    {
        uint8_t pkt_params[6] = {
            0x00, RF_PREAMBLE_LEN,  /* preamble MSB, LSB */
            0x00,                   /* variable length header */
            RF_MAX_PAYLOAD_LEN,     /* maxPayloadLength */
            0x01,                   /* CRC on */
            0x00                    /* IQ standard */
        };
        sx1262_cmd(0x8C, pkt_params, 6);   /* 0x8C = SetPacketParams */
        platform_sx1262_wait_busy(10);
    }

    /* SetTxParams: RF_TX_POWER_DBM (default 22 = SX1262 max), rampTime=40µs.
     * Override via -DRF_TX_POWER_DBM=<x> or in variants/<board>/config.h.
     * The E22-900M30S external PA boosts the on-chip output by ~8 dBm. */
    {
        uint8_t tx_params[2] = { (uint8_t)RF_TX_POWER_DBM, 0x04 };
        sx1262_cmd(0x8E, tx_params, 2);   /* 0x8E = SetTxParams */
        platform_sx1262_wait_busy(10);
    }

    /* SetDioIrqParams: enable TxDone+RxDone+Timeout on DIO1 */
    {
        uint8_t irq_params[8] = {
            0x02, 0x03,   /* irqMask: TxDone(0x0001) | RxDone(0x0002) */
            0x02, 0x03,   /* DIO1 */
            0x00, 0x00,   /* DIO2 */
            0x00, 0x00    /* DIO3 */
        };
        sx1262_cmd(0x08, irq_params, 8);   /* 0x08 = SetDioIrqParams */
        platform_sx1262_wait_busy(10);
    }

    /* WriteRegister 0x08AC = 0x96: RX boosted gain mode.
     * Improves RX sensitivity by ~3 dB at ~2 mA additional current.
     * 0x94 = power-saving (reset default), 0x96 = boosted.
     * Used on all SX1262-based boards in MeshCore, Meshtastic, etc.
     * Reference: SX1262 UM Section 9.6 (RegRxGain). */
    {
        uint8_t wr_params[3] = { 0x08, 0xAC, 0x96 };
        sx1262_cmd(0x0D, wr_params, 3);   /* 0x0D = WriteRegister */
        platform_sx1262_wait_busy(5);
    }

    /* SX1262 errata §15.1: receiver spurious reception fix.
     * For all BW != 500 kHz, bit 2 of reg 0x08B5 must be SET.
     * (RadioLib fixSensitivity(): sensitivityConfig |= 0x04) */
    {
        uint8_t tx[5] = { 0x1D, 0x08, 0xB5, 0x00, 0x00 };  /* 0x1D = ReadRegister */
        uint8_t rx[5] = {0};
        platform_spi_transfer(tx, rx, 5);
        uint8_t reg = rx[4];
        reg |= 0x04u;
        uint8_t wr[3] = { 0x08, 0xB5, reg };
        sx1262_cmd(0x0D, wr, 3);
        platform_sx1262_wait_busy(5);
    }

    /* SX1262 errata §15.2: PA clamp fix — overly eager PA clamping
     * during initial NFET turn-on causes weak / failed TX without this.
     * (RadioLib fixPaClamping(): clampConfig |= 0x1E on reg 0x08D8) */
    {
        uint8_t tx[5] = { 0x1D, 0x08, 0xD8, 0x00, 0x00 };  /* 0x1D = ReadRegister */
        uint8_t rx[5] = {0};
        platform_spi_transfer(tx, rx, 5);
        uint8_t reg = rx[4];
        reg |= 0x1Eu;
        uint8_t wr[3] = { 0x08, 0xD8, reg };
        sx1262_cmd(0x0D, wr, 3);
        platform_sx1262_wait_busy(5);
    }
}

void radio_init(void)
{
    RIVR_LOGI(TAG, "radio_init: resetting SX1262");

    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));

    platform_sx1262_reset();
    platform_sx1262_wait_busy(100);
    radio_apply_config();

    /* Attach DIO1 ISR — done ONCE on first boot only.
     * radio_hard_reset() skips this step to avoid double-registration. */
    gpio_isr_handler_add(PIN_SX1262_DIO1, radio_isr, NULL);

    RIVR_LOGI(TAG, "radio_init: done (SF%u BW%skHz @ %luHz)",
             RF_SPREADING_FACTOR, RF_BANDWIDTH_DISPLAY_STR, (unsigned long)RF_FREQ_HZ);
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
    s_last_rx_event_ms = tb_millis();  /* arm RX-silence timer */
    RIVR_LOGI(TAG, "RX mode started");
}

void radio_hard_reset(void)
{
    g_rivr_metrics.radio_hard_reset++;
    s_tx_fail_streak      = 0;
    s_spurious_irq_streak = 0;
    s_busy_stuck_streak   = 0;
    s_in_rx               = false;
    s_last_rx_event_ms    = 0u;  /* re-armed by radio_start_rx() */
    RIVR_LOGW(TAG, "hard reset #%" PRIu32 " — re-initialising SX1262",
              g_rivr_metrics.radio_hard_reset);
    platform_sx1262_reset();
    platform_sx1262_wait_busy(100);
    radio_apply_config();
    radio_start_rx();
}

/**
 * @brief Reason codes for radio_guard_reset().
 *
 * Used to increment the per-reason reset counter in g_rivr_metrics and to
 * label the WARN log.  Any new reason must be added to both the enum and the
 * labels array.
 */
typedef enum {
    RADIO_RESET_REASON_BUSY_STUCK    = 0,  /**< BUSY pin stuck high before TX       */
    RADIO_RESET_REASON_TX_TIMEOUT    = 1,  /**< TX HW timeout or SW deadline streak  */
    RADIO_RESET_REASON_SPURIOUS_IRQ  = 2,  /**< Spurious DIO1 event streak           */
    RADIO_RESET_REASON__COUNT               /**< Sentinel — keep last                */
} radio_reset_reason_t;

static const char * const s_reset_reason_labels[RADIO_RESET_REASON__COUNT] = {
    "busy_stuck", "tx_timeout", "spurious_irq"
};

/**
 * @brief Rate-limited wrapper for radio_hard_reset().
 *
 * Enforces a minimum of RADIO_RESET_BACKOFF_MS between consecutive hard
 * resets to prevent reset storms under persistent fault conditions.
 * When the cooldown is active, increments radio_reset_backoff and returns
 * without resetting.
 *
 * On every actual reset (not backoff-denied): increments the per-reason
 * counter in g_rivr_metrics and emits one WARN log.  The backoff itself
 * acts as the rate-limiter so no secondary counter is needed here.
 *
 * @param reason  Why the reset was requested (metric + log).
 */
static void radio_guard_reset(radio_reset_reason_t reason)
{
    const char *label = (reason < RADIO_RESET_REASON__COUNT)
                        ? s_reset_reason_labels[reason] : "unknown";
    uint32_t now = tb_millis();
    if (s_reset_happened && (now - s_last_reset_ms) < RADIO_RESET_BACKOFF_MS) {
        g_rivr_metrics.radio_reset_backoff++;
        RIVR_LOGW(TAG,
            "reset(%s) denied – backoff %" PRIu32 "ms remaining"
            " (denied=%" PRIu32 ")",
            label,
            (uint32_t)(RADIO_RESET_BACKOFF_MS - (now - s_last_reset_ms)),
            g_rivr_metrics.radio_reset_backoff);
        return;
    }
    /* Actual reset — increment per-reason counter then emit one WARN. */
    switch (reason) {
        case RADIO_RESET_REASON_BUSY_STUCK:   g_rivr_metrics.radio_reset_busy_stuck++;   break;
        case RADIO_RESET_REASON_TX_TIMEOUT:   g_rivr_metrics.radio_reset_tx_timeout++;   break;
        case RADIO_RESET_REASON_SPURIOUS_IRQ: g_rivr_metrics.radio_reset_spurious_irq++; break;
        default: break;
    }
    s_reset_happened = true;
    s_last_reset_ms  = now;
    RIVR_LOGW(TAG, "guard_reset(%s) – hard reset #%" PRIu32 " (reason=%s)",
              label, g_rivr_metrics.radio_hard_reset + 1u, label);
    radio_hard_reset();
}

/**
 * @brief Check for prolonged RX silence; call once per main-loop iteration.
 *
 * If the radio has been in continuous-RX for longer than RADIO_RX_SILENCE_MS
 * without any DIO1 event, increment the observability counter and emit one
 * WARN log. This no longer triggers a radio reset because ordinary mesh
 * silence is not treated as a hardware fault.
 */
void radio_check_timeouts(void)
{
    uint32_t now = tb_millis();

    /* Arm the silence baseline on first call (avoids false alarm at boot). */
    if (s_last_rx_event_ms == 0u) {
        s_last_rx_event_ms = now;
        return;
    }

    /* Only meaningful while the radio is in continuous-RX. */
    if (!s_in_rx) return;

    if ((now - s_last_rx_event_ms) >= RADIO_RX_SILENCE_MS) {
        g_rivr_metrics.radio_rx_timeout++;
        RIVR_LOGW(TAG,
            "RX silent %" PRIu32 "ms – no reset (total=%" PRIu32 ")",
            (uint32_t)(now - s_last_rx_event_ms),
            g_rivr_metrics.radio_rx_timeout);
        s_last_rx_event_ms = now;   /* prevent immediate re-trigger */
    }
}

#ifdef RIVR_FAULT_INJECT
/**
 * @brief Reset all internal radio statics for test isolation.
 * Call between test cases to ensure a clean initial state.
 */
void radio_fault_reset_state(void)
{
    s_reset_happened    = false;
    s_last_reset_ms     = 0u;
    s_last_rx_event_ms  = 0u;
    s_busy_stuck_streak = 0u;
    s_tx_fail_streak    = 0u;
    s_spurious_irq_streak = 0u;
    s_in_rx             = false;
    s_dio1_pending      = false;
    g_fault_busy_stuck  = false;
    g_fault_tx_no_done  = false;
    g_fault_rx_silence  = false;
    g_fault_crc_fail    = 0u;
    memset(&g_rivr_metrics, 0, sizeof(g_rivr_metrics));
}
#endif /* RIVR_FAULT_INJECT */

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
    portENTER_CRITICAL_ISR(&s_dio1_mux);
    s_dio1_pending = true;
    portEXIT_CRITICAL_ISR(&s_dio1_mux);
}

/* Called from the main loop after every rivr_tick().  Drains all pending
 * DIO1 events (there is normally at most one per loop iteration). */
void radio_service_rx(void)
{
#ifdef RIVR_FAULT_INJECT
    if (g_fault_rx_silence) return;
#endif
    /* Atomically read and clear the flag — prevents a dual-core race where
     * the ISR on CPU1 sets the flag after the check but before the clear. */
    bool pending;
    portENTER_CRITICAL(&s_dio1_mux);
    pending        = s_dio1_pending;
    s_dio1_pending = false;
    portEXIT_CRITICAL(&s_dio1_mux);
    if (!pending) return;
#ifdef RIVR_FAULT_INJECT
    /* Burst CRC-error injection: simulate PayloadCrcError for the next N frames.
     * s_dio1_pending was already cleared atomically above — no need to clear it again. */
    if (g_fault_crc_fail > 0u) {
        g_fault_crc_fail--;
        s_last_rx_event_ms = tb_millis();
        g_rivr_metrics.radio_rx_crc_fail++;
        uint32_t n = g_rivr_metrics.radio_rx_crc_fail;
        if ((n & (n - 1u)) == 0u) {
            RIVR_LOGW(TAG, "RX CRC error (fault-injected) – discarded (total=%" PRIu32 ")", n);
        }
        return;
    }
#endif
    /* s_dio1_pending was already cleared atomically above. */
    s_last_rx_event_ms = tb_millis();   /* reset RX-silence watchdog */

    /* 1. Read IRQ status */
    uint8_t irq_status[2] = {0};
    sx1262_read_cmd(0x12, irq_status, 2);   /* 0x12 = GetIrqStatus */
    uint16_t irq = ((uint16_t)irq_status[0] << 8) | irq_status[1];

    /* 2. Clear all IRQ flags */
    uint8_t clr[2] = { irq_status[0], irq_status[1] };
    sx1262_cmd(0x02, clr, 2);   /* 0x02 = ClearIrqStatus */

    /* 3. Detect spurious events (neither TxDone nor RxDone).  Five in a row
     *    without a valid frame suggests the radio has lost its IRQ state. */
    if (!(irq & 0x0003)) {
        if (++s_spurious_irq_streak >= 5u) {
            RIVR_LOGW(TAG, "5 spurious DIO1 events – triggering guarded reset");
            radio_guard_reset(RADIO_RESET_REASON_SPURIOUS_IRQ);
        }
        return;
    }

    if (!(irq & 0x0002)) return;  /* Not RxDone – ignore (TxDone, Timeout, etc.) */

    s_spurious_irq_streak = 0;    /* reset on valid RxDone */

    /* PayloadCrcError (bit 6 = 0x0040): SX1262 sets this alongside RxDone
     * when the received frame fails hardware CRC.  Discard immediately —
     * corrupted data in the buffer is meaningless.  Log on first and every
     * power-of-2 occurrence (logarithmic rate limiting). */
    if (irq & 0x0040) {
        g_rivr_metrics.radio_rx_crc_fail++;
        uint32_t n = g_rivr_metrics.radio_rx_crc_fail;
        if ((n & (n - 1u)) == 0u) {   /* power of 2: 1,2,4,8,16,… */
            RIVR_LOGW(TAG, "RX CRC error – frame discarded (total=%" PRIu32 ")", n);
        }
        return;
    }

    /* 3. GetRxBufferStatus → payloadLen, startAddr */
    uint8_t buf_status[2] = {0};
    sx1262_read_cmd(0x13, buf_status, 2);   /* 0x13 = GetRxBufferStatus */
    uint8_t payload_len  = buf_status[0];
    uint8_t start_addr   = buf_status[1];

    if (payload_len == 0) return;
    /* RF_MAX_PAYLOAD_LEN == 255 == UINT8_MAX; guard against 0-len only */

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

    /* ── SX1262 TX SEQUENCE (per UM §13.1.1) ───────────────────────────────
     *
     * CORRECT ORDER (enforced here):
     *   1) BUSY low            — chip is ready for SPI
     *   2) SetStandby(RC)      — exit continuous-RX before any data ops
     *   3) ClearIrqStatus      — discard stale TxDone/RxDone from prior ops
     *   4) SetPacketParams     — update payload length for this frame
     *   5) WriteBuffer         — load payload while in Standby (not RX!)
     *   6) RF-switch → TX      — assert TXEN / deassert RXEN
     *   7) SetTx(timeout)      — arm PA and start transmission
     *   8) poll TxDone / HW-timeout IRQ
     *   9) radio_start_rx()    — return chip to RX on completion
     *
     * ROOT CAUSE (fixed in this commit):
     *   The previous sequence called WriteBuffer BEFORE SetStandby.  While in
     *   continuous-RX the SX1262 state machine is actively writing received
     *   bytes into the RX FIFO; a concurrent WriteBuffer over SPI may silently
     *   corrupt the TX FIFO pointer or be ignored.  The subsequent SetTx then
     *   fired with stale or empty payload, producing no detectable RF output
     *   while still asserting TxDone (because the chip completed the command).
     *
     * ── Step 1: BUSY check ─────────────────────────────────────────────── */
    if (!platform_sx1262_wait_busy(10)) {
        g_rivr_metrics.radio_busy_stall++;
        g_rivr_metrics.radio_busy_timeout_total++;
        uint32_t _n = g_rivr_metrics.radio_busy_timeout_total;
        if ((_n & (_n - 1u)) == 0u) {   /* log on 1,2,4,8,… */
            RIVR_LOGW(TAG, "busy_stuck before TX (total=%" PRIu32 ")", _n);
        }
        if (++s_busy_stuck_streak >= RADIO_BUSY_STUCK_MAX) {
            s_busy_stuck_streak = 0u;
            radio_guard_reset(RADIO_RESET_REASON_BUSY_STUCK);
        }
        return false;
    }
    s_busy_stuck_streak = 0u;   /* BUSY went low – clear streak */

    /* ── Step 2: SetStandby(RC) — exit RX before touching the FIFO ──────── */
    {
        uint8_t stdby = 0x00;
        sx1262_cmd(0x80, &stdby, 1);   /* 0x80 = SetStandby(STDBY_RC) */
        platform_sx1262_wait_busy(5);
    }
    s_in_rx = false;    /* update flag immediately; RF switch follows below */

    /* ── Step 3: ClearIrqStatus(0xFFFF) — flush stale TxDone/RxDone ──────
     * Without this a residual TxDone bit from a previously-aborted TX can
     * cause the poll loop below to return true on the very first iteration
     * before the PA has even fired, producing a false "tx_ok".             */
    {
        uint8_t clr[2] = { 0xFF, 0xFF };
        sx1262_cmd(0x02, clr, 2);       /* 0x02 = ClearIrqStatus */
        platform_sx1262_wait_busy(5);
    }

    /* ── Step 4: SetPacketParams — update payload length for this frame ─── */
    {
        uint8_t pkt_params[6] = {
            0x00, RF_PREAMBLE_LEN,  /* preamble MSB, LSB */
            0x00,                   /* variable-length header */
            req->len,               /* actual payload length */
            0x01,                   /* CRC on */
            0x00                    /* IQ standard */
        };
        sx1262_cmd(0x8C, pkt_params, 6);   /* 0x8C = SetPacketParams */
        platform_sx1262_wait_busy(5);
    }

    /* ── Step 5: WriteBuffer — load payload now that chip is in Standby ───
     * WriteBuffer takes [opcode][offset][data…] with NO NOP byte between
     * offset and data (unlike ReadBuffer which inserts one extra NOP byte).
     * A spurious NOP here would prepend a null byte to every TX frame,
     * corrupting byte[0] of the RIVR magic header.                         */
    {
        uint8_t tx_cmd[2 + RF_MAX_PAYLOAD_LEN];
        tx_cmd[0] = 0x0E;   /* WriteBuffer */
        tx_cmd[1] = 0x00;   /* TX FIFO base address */
        memcpy(tx_cmd + 2, req->data, req->len);
        platform_spi_transfer(tx_cmd, NULL, 2 + req->len);
        platform_sx1262_wait_busy(5);
    }

    /* ── Step 6: RF-switch → TX path (RXEN low, TXEN high via DIO2) ─────── */
    platform_sx1262_set_rxen(false);

    /* ── Step 7: SetTx ───────────────────────────────────────────────────────
     * Timeout = ToA × 2 in SX1262 ticks (15.625 µs each).
     *   N = toa_us × 2 / 15.625 = toa_us × 128 / 1000 = toa_us × 16 / 125
     * 2× headroom prevents premature HW-timeout IRQ on long SF12 frames.   */
    uint32_t timeout_ticks = (req->toa_us * 16u) / 125u;
    {
        uint8_t tx_timeout[3] = {
            (uint8_t)(timeout_ticks >> 16),
            (uint8_t)(timeout_ticks >>  8),
            (uint8_t)(timeout_ticks)
        };
        sx1262_cmd(0x83, tx_timeout, 3);   /* 0x83 = SetTx */
    }
    ESP_LOGD(TAG, "tx_start: len=%u toa_us=%lu ticks=%lu",
             (unsigned)req->len,
             (unsigned long)req->toa_us,
             (unsigned long)timeout_ticks);

    /* ── Step 8: poll for TxDone (up to toa_us × 2 + 100 ms) ─────────────── */
    uint32_t t0 = tb_millis();
    uint32_t deadline_ms = t0 + req->toa_us / 1000u * 2u + 100u;

    while (tb_millis() < deadline_ms) {
        esp_task_wdt_reset();  /* poll can take up to toa_us×2 — prevent WDT trip at high SF */
        uint8_t irq[2] = {0};
        sx1262_read_cmd(0x12, irq, 2);   /* 0x12 = GetIrqStatus */
        uint16_t flags = ((uint16_t)irq[0] << 8) | irq[1];
#ifdef RIVR_FAULT_INJECT
        if (g_fault_tx_no_done) flags &= ~0x0001u;   /* suppress TxDone */
#endif
        if (flags & 0x0001) {   /* TxDone */
            uint8_t clr[2] = { irq[0], irq[1] };
            sx1262_cmd(0x02, clr, 2);   /* 0x02 = ClearIrqStatus */
            s_tx_fail_streak = 0;       /* clear failure streak on success */
            ESP_LOGD(TAG, "tx_done: len=%u elapsed_ms=%lu",
                     (unsigned)req->len,
                     (unsigned long)(tb_millis() - t0));
            radio_start_rx();   /* ── Step 9: return to RX */
            return true;
        }
        if (flags & 0x0200) {   /* SX1262 HW Timeout IRQ */
            g_rivr_metrics.radio_tx_fail++;       /* legacy aggregate counter */
            g_rivr_metrics.tx_timeout_total++;    /* Step-9: HW-timeout split  */
            RIVR_LOGW(TAG, "tx_hw_timeout: streak %u total_tmo=%" PRIu32,
                      (unsigned)(s_tx_fail_streak + 1u),
                      g_rivr_metrics.tx_timeout_total);
            if (++s_tx_fail_streak >= 3u) {
                radio_guard_reset(RADIO_RESET_REASON_TX_TIMEOUT);
            } else {
                radio_start_rx();
            }
            return false;
        }
    }
    /* SW poll deadline exceeded (toa_us×2 + 100 ms elapsed without TxDone) */
    g_rivr_metrics.radio_tx_fail++;        /* legacy aggregate counter */
    g_rivr_metrics.tx_deadline_total++;    /* Step-9: SW-deadline split */
    RIVR_LOGW(TAG, "tx_sw_deadline: streak %u total_ddl=%" PRIu32,
              (unsigned)(s_tx_fail_streak + 1u),
              g_rivr_metrics.tx_deadline_total);
    if (++s_tx_fail_streak >= 3u) {
        radio_guard_reset(RADIO_RESET_REASON_TX_TIMEOUT);
    } else {
        radio_start_rx();
    }
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
