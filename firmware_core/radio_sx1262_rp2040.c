/**
 * @file  firmware_core/radio_sx1262_rp2040.c
 * @brief SX1262 radio driver for RP2040.
 *
 * Derived from Rivr's nRF52 SX1262 path, with the same shared packet/ring
 * buffer logic but a RP2040 Arduino HAL and no FreeRTOS dependency.
 */

#if defined(RIVR_PLATFORM_RP2040)

#include "radio_sx1262.h"
#include "platform_rp2040.h"
#include "timebase.h"
#include "rivr_metrics.h"
#include "rivr_log.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "RADIO"
#define RADIO_RX_SILENCE_MS 60000u

static uint32_t s_last_rx_event_ms = 0u;
static volatile bool s_in_rx = false;
static volatile atomic_bool s_dio1_pending = ATOMIC_VAR_INIT(false);
/* Adaptive frequency correction: tracks the current synthesiser centre frequency
 * after per-packet FEI-based nudges (mirrors RadioLib getFrequencyError autoCorrect). */
static int32_t s_corrected_freq_hz = (int32_t)RIVR_RF_FREQ_HZ;

static void s_radio_isr_trampoline(void)
{
    atomic_store_explicit(&s_dio1_pending, true, memory_order_release);
}

void radio_isr(void *arg)
{
    (void)arg;
    s_radio_isr_trampoline();
}

static void sx_write_cmd(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint8_t buf[64];
    uint8_t rx[64];

    buf[0] = cmd;
    if (data && len) {
        memcpy(&buf[1], data, len);
    }

    platform_sx1262_wait_busy(10);
    platform_spi_transfer(buf, rx, (uint16_t)(1u + len));
}

static void sx_read_cmd(uint8_t cmd, uint8_t *out, uint8_t out_len)
{
    uint8_t tx[65] = {cmd, 0x00};
    uint8_t rx[65];

    platform_sx1262_wait_busy(10);
    platform_spi_transfer(tx, rx, (uint16_t)(2u + out_len));
    memcpy(out, &rx[2], out_len);
}

static void sx_write_reg(uint16_t addr, uint8_t val)
{
    uint8_t d[3] = {(uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), val};
    sx_write_cmd(0x0D, d, 3);
}

static void sx_read_reg(uint16_t addr, uint8_t *out, uint8_t len)
{
    uint8_t tx[4 + 255] = {0};
    uint8_t rx[4 + 255] = {0};

    tx[0] = 0x1D;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)(addr & 0xFF);
    tx[3] = 0x00;

    platform_sx1262_wait_busy(10);
    platform_spi_transfer(tx, rx, (uint16_t)(4u + len));
    memcpy(out, &rx[4], len);
}

static void sx_write_buf(uint8_t offset, const uint8_t *data, uint8_t len)
{
    uint8_t tx[2 + 255];
    uint8_t rx[2 + 255];

    tx[0] = 0x0E;
    tx[1] = offset;
    if (len > 0u && data) {
        memcpy(&tx[2], data, len);
    }
    platform_sx1262_wait_busy(10);
    platform_spi_transfer(tx, rx, (uint16_t)(2u + len));
}

static void sx_read_buf(uint8_t offset, uint8_t *data, uint8_t len)
{
    uint8_t tx[3 + 255];
    uint8_t rx[3 + 255];

    memset(tx, 0x00, 3u + len);
    tx[0] = 0x1E;
    tx[1] = offset;
    tx[2] = 0x00;
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

static void sx_calibrate_image(uint32_t freq_hz)
{
    uint8_t d[2];

    if (freq_hz >= 430000000UL && freq_hz <= 440000000UL) {
        d[0] = 0x6Bu; d[1] = 0x6Fu;
    } else if (freq_hz >= 470000000UL && freq_hz <= 510000000UL) {
        d[0] = 0x75u; d[1] = 0x81u;
    } else if (freq_hz >= 779000000UL && freq_hz <= 787000000UL) {
        d[0] = 0xC1u; d[1] = 0xC5u;
    } else if (freq_hz >= 863000000UL && freq_hz <= 870000000UL) {
        d[0] = 0xD7u; d[1] = 0xDBu;
    } else if (freq_hz >= 902000000UL && freq_hz <= 928000000UL) {
        d[0] = 0xE1u; d[1] = 0xE9u;
    } else {
        return;
    }

    sx_write_cmd(0x98, d, 2);
    platform_sx1262_wait_busy(20);
}

static void sx_apply_waveshare_tweaks(void)
{
    /* MeshCore and Meshtastic both run this board with SX1262 OCP at 140 mA,
     * DIO2 RF switching enabled, and boosted RX gain. */
    sx_write_reg(0x08E7u, 0x38u);  /* OCP = 140 mA */
    sx_write_reg(0x08ACu, 0x96u);  /* boosted RX gain */

    /* SX1262 errata §15.1: receiver spurious reception fix.
     * For all BW != 500 kHz, bit 2 of reg 0x08B5 must be SET.
     * (RadioLib fixSensitivity(): sensitivityConfig |= 0x04) */
    {
        uint8_t reg = 0u;
        sx_read_reg(0x08B5u, &reg, 1u);
        reg |= 0x04u;
        sx_write_reg(0x08B5u, reg);
    }

    /* SX1262 errata §15.2: PA clamp fix — overly eager PA clamping
     * during initial NFET turn-on causes weak / failed TX without this.
     * (RadioLib fixPaClamping(): clampConfig |= 0x1E on reg 0x08D8) */
    {
        uint8_t reg = 0u;
        sx_read_reg(0x08D8u, &reg, 1u);
        reg |= 0x1Eu;
        sx_write_reg(0x08D8u, reg);
    }
}

static rf_rx_frame_t s_rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t s_tx_storage[RF_TX_QUEUE_CAP];
rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

void radio_init_buffers_only(void)
{
    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue, s_tx_storage, RF_TX_QUEUE_CAP, sizeof(rf_tx_request_t));
    RIVR_LOGI(TAG, "radio_init_buffers_only: ringbufs ready");
}

void radio_init(void)
{
    radio_init_buffers_only();

    platform_sx1262_reset();
    platform_sx1262_wait_busy(100);
    sx_set_standby();
    { uint8_t d = 0x01; sx_write_cmd(0x96, &d, 1); }

#if defined(RIVR_SX1262_USE_DIO3_TCXO) && RIVR_SX1262_USE_DIO3_TCXO
    {
        uint8_t d[4] = {
            (uint8_t)RIVR_SX1262_TCXO_VOLTAGE,
            (uint8_t)(RIVR_SX1262_TCXO_DELAY_TICKS >> 16),
            (uint8_t)(RIVR_SX1262_TCXO_DELAY_TICKS >> 8),
            (uint8_t)(RIVR_SX1262_TCXO_DELAY_TICKS)
        };
        sx_write_cmd(0x97, d, 4);
    }
#endif

    { uint8_t d = 0x7F; sx_write_cmd(0x89, &d, 1); platform_sx1262_wait_busy(100); }
    sx_set_standby();

    { uint8_t d = 0x01; sx_write_cmd(0x8A, &d, 1); }

    {
        uint32_t frf = (uint32_t)(((uint64_t)RIVR_RF_FREQ_HZ << 25) / 32000000UL);
        uint8_t d[4] = {
            (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
            (uint8_t)(frf >> 8), (uint8_t)(frf)
        };
        sx_write_cmd(0x86, d, 4);
    }
    s_corrected_freq_hz = (int32_t)RIVR_RF_FREQ_HZ;
    sx_calibrate_image(RIVR_RF_FREQ_HZ);

    { uint8_t d[4] = {0x04, 0x07, 0x00, 0x01}; sx_write_cmd(0x95, d, 4); }
    { uint8_t d[2] = {(uint8_t)RF_TX_POWER_DBM, 0x04}; sx_write_cmd(0x8E, d, 2); }
    { uint8_t d = 0x01; sx_write_cmd(0x9D, &d, 1); }
    { uint8_t d[2] = {0x00, 0x00}; sx_write_cmd(0x8F, d, 2); }

    {
        uint8_t bw;
        uint8_t cr_reg;
        switch (RF_BANDWIDTH_HZ) {
            case 7800: bw = 0x00; break;
            case 10400: bw = 0x08; break;
            case 15600: bw = 0x01; break;
            case 20800: bw = 0x09; break;
            case 31250: bw = 0x02; break;
            case 41700: bw = 0x0A; break;
            case 62500: bw = 0x03; break;
            case 125000: bw = 0x04; break;
            case 250000: bw = 0x05; break;
            case 500000: bw = 0x06; break;
            default: bw = 0x03; break;
        }
        switch (RF_CODING_RATE) {
            case 5: cr_reg = 0x01u; break;
            case 6: cr_reg = 0x02u; break;
            case 7: cr_reg = 0x03u; break;
            case 8: cr_reg = 0x04u; break;
            default: cr_reg = 0x01u; break;
        }
        uint8_t ldro = (RF_SPREADING_FACTOR >= 11 && RF_BANDWIDTH_HZ <= 125000) ? 1u : 0u;
        uint8_t d[4] = {
            (uint8_t)RF_SPREADING_FACTOR,
            bw,
            cr_reg,
            ldro
        };
        sx_write_cmd(0x8B, d, 4);
    }

    {
        uint8_t d[6] = {0x00, RF_PREAMBLE_LEN, 0x00, RF_MAX_PAYLOAD_LEN, 0x01, 0x00};
        sx_write_cmd(0x8C, d, 6);
    }

    {
        uint8_t d[8] = {0x02, 0x03, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00};
        sx_write_cmd(0x08, d, 8);
    }

    sx_apply_waveshare_tweaks();
    platform_dio1_attach_isr(s_radio_isr_trampoline);

    {
        uint8_t st[1] = {0u};
        sx_read_cmd(0xC0, st, 1);
        RIVR_LOGI(TAG, "radio_init: GetStatus=0x%02X", st[0]);
    }

    RIVR_LOGI(TAG, "radio_init: SX1262 ready (%u Hz, SF%u, BW%u, CR4/%u, +%u dBm)",
              (unsigned)RIVR_RF_FREQ_HZ,
              (unsigned)RF_SPREADING_FACTOR,
              (unsigned)RF_BANDWIDTH_HZ,
              (unsigned)RF_CODING_RATE,
              (unsigned)RF_TX_POWER_DBM);
}

void radio_start_rx(void)
{
    /* SX1262 errata §15.4: IQ polarity — reapply before each RX.
     * SetPacketParams resets register 0x0736 bit 2 to the inverted state.
     * For non-inverted (peer-to-peer) LoRa, bit 2 must be SET.
     * RadioLib fixes this via fixInvertedIQ(false) inside setPacketParams(),
     * called from startReceiveCommon() on every RX entry. */
    {
        uint8_t iq = 0u;
        sx_read_reg(0x0736u, &iq, 1u);
        iq |= 0x04u;
        sx_write_reg(0x0736u, iq);
    }

    platform_sx1262_set_rxen(true);
    vTaskDelay(pdMS_TO_TICKS(1));
    {
        uint8_t d[3] = {0xFF, 0xFF, 0xFF};
        sx_write_cmd(0x82, d, 3);
    }
    platform_sx1262_wait_busy(20);
    s_in_rx = true;
    s_last_rx_event_ms = tb_millis();
}

void radio_service_rx(void)
{
    if (!atomic_load_explicit(&s_dio1_pending, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&s_dio1_pending, false, memory_order_release);

    {
        uint8_t irq_bytes[2];
        sx_read_cmd(0x12, irq_bytes, 2);
        uint16_t irq = ((uint16_t)irq_bytes[0] << 8) | irq_bytes[1];

        { uint8_t d[2] = {irq_bytes[0], irq_bytes[1]}; sx_write_cmd(0x02, d, 2); }

        if (!(irq & 0x0002u)) {
            /* DIO1 fired but RxDone not set — spurious TxDone/Timeout IRQ.
             * Restart RX to ensure the radio doesn't stay in STDBY if a
             * Timeout fired (rare with infinite RX, but safe to handle). */
            RIVR_LOGD(TAG, "DIO1 non-RxDone IRQ=0x%04X – restarting RX", irq);
            goto restart_rx;
        }

        if (irq & 0x0040u) {
            g_rivr_metrics.radio_rx_crc_fail++;
            uint32_t n = g_rivr_metrics.radio_rx_crc_fail;
            if ((n & (n - 1u)) == 0u) {
                RIVR_LOGW(TAG, "RX CRC error - frame discarded (total=%" PRIu32 ")", n);
            }
            return;
        }

        if (irq & 0x0002u) {
            uint8_t rxstat[2];
            sx_read_cmd(0x13, rxstat, 2);
            uint8_t pkt_len = rxstat[0];
            uint8_t buf_offset = rxstat[1];

            if (pkt_len == 0u || pkt_len > RF_MAX_PAYLOAD_LEN) {
                goto restart_rx;
            }

            rf_rx_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            frame.len = pkt_len;

            sx_read_buf(buf_offset, frame.data, pkt_len);

            {
                uint8_t pstat[3];
                sx_read_cmd(0x14, pstat, 3);
                frame.rssi_dbm = (int8_t)(-(pstat[0] >> 1));
                frame.snr_db = (int8_t)((int8_t)pstat[1] >> 2);
                frame.rx_mono_ms = tb_millis();
            }

            rb_try_push(&rf_rx_ringbuf, &frame);
            s_last_rx_event_ms = tb_millis();

            /* Adaptive frequency error correction (mirrors RadioLib SX126x::getFrequencyError
             * with autoCorrect). After each valid packet the SX1262 FEI registers
             * 0x076B-0x076D hold a 20-bit signed frequency error. Reading and applying
             * it keeps the synthesiser locked onto a crystal-drifted peer (e.g. LilyGo
             * SX1276) across successive packets at BW=62.5 kHz.
             * Formula from RadioLib: error_hz = 1.55 × raw × BW_kHz / 1600             */
            {
                uint8_t fei[3] = {0u, 0u, 0u};
                sx_read_reg(0x076Bu, fei, 3u);
                int32_t raw = (int32_t)(((uint32_t)fei[0] << 16) |
                                        ((uint32_t)fei[1] <<  8) |
                                         (uint32_t)fei[2]);
                raw &= (int32_t)0x000FFFFF;
                if (raw & (int32_t)0x00080000) {
                    raw |= (int32_t)0xFFF00000;  /* sign-extend 20→32 bit */
                }
                float err_hz = 1.55f * (float)raw
                               * (float)RF_BANDWIDTH_KHZ / 1600.0f;
                s_corrected_freq_hz -= (int32_t)err_hz;
                /* Clamp to ±50 kHz around nominal to prevent runaway. */
                if (s_corrected_freq_hz < (int32_t)(RIVR_RF_FREQ_HZ - 50000u))
                    s_corrected_freq_hz = (int32_t)(RIVR_RF_FREQ_HZ - 50000u);
                if (s_corrected_freq_hz > (int32_t)(RIVR_RF_FREQ_HZ + 50000u))
                    s_corrected_freq_hz = (int32_t)(RIVR_RF_FREQ_HZ + 50000u);
                uint32_t frf = (uint32_t)(((uint64_t)(uint32_t)s_corrected_freq_hz << 25)
                                           / 32000000UL);
                uint8_t fd[4] = {
                    (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
                    (uint8_t)(frf >>  8), (uint8_t)(frf)
                };
                sx_write_cmd(0x86u, fd, 4u);
            }
        }

        return;
    }

restart_rx:
    radio_start_rx();
}

bool radio_transmit(const rf_tx_request_t *req)
{
    if (!req || req->len == 0u) {
        return false;
    }

    platform_sx1262_wait_busy(10);
    sx_set_standby();
    { uint8_t d[2] = {0xFF, 0xFF}; sx_write_cmd(0x02, d, 2); }

    {
        uint8_t d[6] = {0x00, RF_PREAMBLE_LEN, 0x00, req->len, 0x01, 0x00};
        sx_write_cmd(0x8C, d, 6);
    }

    /* SX1262 errata §15.4: IQ polarity — reapply after SetPacketParams for TX. */
    {
        uint8_t iq = 0u;
        sx_read_reg(0x0736u, &iq, 1u);
        iq |= 0x04u;
        sx_write_reg(0x0736u, iq);
    }

    sx_write_buf(0, req->data, req->len);
    platform_sx1262_set_rxen(false);
    vTaskDelay(pdMS_TO_TICKS(1));

    {
        uint32_t timeout_ticks = (req->toa_us * 16u) / 125u;
        uint8_t d[3] = {
            (uint8_t)(timeout_ticks >> 16),
            (uint8_t)(timeout_ticks >> 8),
            (uint8_t)(timeout_ticks)
        };
        sx_write_cmd(0x83, d, 3);
    }

    {
        uint32_t t0 = tb_millis();
        while (true) {
            uint8_t irq_bytes[2];
            sx_read_cmd(0x12, irq_bytes, 2);
            uint16_t irq = ((uint16_t)irq_bytes[0] << 8) | irq_bytes[1];
            if (irq & 0x0001u) {
                uint8_t d[2] = {0xFF, 0xFF};
                sx_write_cmd(0x02, d, 2);
                break;
            }
            if ((tb_millis() - t0) > 2000u) {
                uint8_t st[1] = {0u};
                sx_read_cmd(0xC0, st, 1);
                RIVR_LOGE(TAG, "TX timeout irq=0x%02X%02X status=0x%02X",
                          irq_bytes[0], irq_bytes[1], st[0]);
                g_rivr_metrics.radio_tx_fail++;
                radio_start_rx();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    radio_start_rx();
    return true;
}

void radio_check_timeouts(void)
{
    uint32_t now = tb_millis();

    if (s_last_rx_event_ms == 0u) {
        s_last_rx_event_ms = now;
        return;
    }

    if (!s_in_rx) {
        return;
    }

    if ((now - s_last_rx_event_ms) >= RADIO_RX_SILENCE_MS) {
        g_rivr_metrics.radio_rx_timeout++;
        RIVR_LOGW(TAG,
                  "RX silent %" PRIu32 "ms – restarting RX (total=%" PRIu32 ")",
                  (uint32_t)(now - s_last_rx_event_ms),
                  g_rivr_metrics.radio_rx_timeout);
        radio_start_rx();
    }
}

#endif /* RIVR_PLATFORM_RP2040 */
