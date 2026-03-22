/**
 * @file  radio_lr1110_nrf52.cpp
 * @brief LR1110 LoRa radio driver for nRF52840 variants.
 *
 * Implements the same public API as radio_sx1262.c so Rivr's higher layers
 * remain unchanged. This implementation is nRF52-only and uses RadioLib's
 * LR1110 support, following the same initialization shape MeshCore uses for
 * the SenseCAP T1000-E.
 */

#if defined(RIVR_PLATFORM_NRF52840) && defined(RIVR_RADIO_LR1110) && RIVR_RADIO_LR1110

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <FreeRTOS.h>
#include <task.h>
#include <inttypes.h>
#include <string.h>

extern "C" {
#include "radio_sx1262.h"
#include "rivr_log.h"
#include "rivr_metrics.h"
#include "timebase.h"
}

#define TAG "RADIO"

#ifndef LR1110_TCXO_VOLTAGE
#  define LR1110_TCXO_VOLTAGE 1.6f
#endif

/* RadioLib wrapper: copied in spirit from MeshCore to recover cleanly after
 * header errors and to expose state helpers we need in Rivr. */
class RivrCustomLR1110 : public LR1110 {
public:
    explicit RivrCustomLR1110(Module *mod) : LR1110(mod) {}

    size_t getPacketLength(bool update = true) override {
        size_t len = LR1110::getPacketLength(update);
        if (len == 0u && (getIrqStatus() & RADIOLIB_LR11X0_IRQ_HEADER_ERR)) {
            standby();
        }
        return len;
    }

    int16_t getInstantRssi(float *rssi) {
        return getRssiInst(rssi);
    }
};

static Module s_module(PIN_LR1110_NSS, PIN_LR1110_DIO1, PIN_LR1110_RESET,
                       PIN_LR1110_BUSY, SPI);
static RivrCustomLR1110 s_radio(&s_module);

static rf_rx_frame_t   s_rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t s_tx_storage[RF_TX_QUEUE_CAP];
rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

static volatile bool s_dio1_pending = false;
static volatile bool s_in_rx        = false;
static uint32_t      s_last_rx_ms   = 0u;

static void s_radio_isr_trampoline(void)
{
    s_dio1_pending = true;
}

extern "C" void radio_isr(void *arg)
{
    (void)arg;
    s_radio_isr_trampoline();
}

extern "C" void radio_init_buffers_only(void)
{
    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));
    RIVR_LOGI(TAG, "radio_init_buffers_only: ringbufs ready");
}

extern "C" void radio_init(void)
{
    radio_init_buffers_only();

    SPI.setPins(PIN_LR1110_MISO, PIN_LR1110_SCK, PIN_LR1110_MOSI);
    SPI.begin();

    const float freq_mhz = (float)RF_FREQ_HZ / 1000000.0f;
    const float bw_khz   = (float)RF_BANDWIDTH_HZ / 1000.0f;
    int status = s_radio.begin(freq_mhz, bw_khz, RF_SPREADING_FACTOR,
                               RF_CODING_RATE, RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE,
                               RF_TX_POWER_DBM, RF_PREAMBLE_LEN, LR1110_TCXO_VOLTAGE);
    if (status != RADIOLIB_ERR_NONE) {
        RIVR_LOGE(TAG, "LR1110 begin failed: %d", status);
        return;
    }

    s_radio.setCRC(2);
    s_radio.explicitHeader();
    attachInterrupt(digitalPinToInterrupt(PIN_LR1110_DIO1),
                    s_radio_isr_trampoline, RISING);

    s_in_rx      = false;
    s_last_rx_ms = tb_millis();
    RIVR_LOGI(TAG, "radio_init: LR1110 ready (%u Hz, SF%u, BW%u, CR4/%u, +%u dBm)",
              (unsigned)RF_FREQ_HZ, (unsigned)RF_SPREADING_FACTOR,
              (unsigned)RF_BANDWIDTH_HZ, (unsigned)RF_CODING_RATE,
              (unsigned)RF_TX_POWER_DBM);
}

extern "C" void radio_start_rx(void)
{
    int err = s_radio.startReceive();
    if (err != RADIOLIB_ERR_NONE) {
        RIVR_LOGW(TAG, "startReceive failed: %d", err);
        return;
    }
    s_in_rx = true;
}

extern "C" void radio_hard_reset(void)
{
    s_in_rx = false;
    radio_init();
    radio_start_rx();
    g_rivr_metrics.radio_hard_reset++;
}

extern "C" bool radio_transmit(const rf_tx_request_t *req)
{
    if (!req || req->len == 0u) return false;

    s_in_rx = false;
    s_dio1_pending = false;

    int err = s_radio.transmit(req->data, req->len);
    if (err != RADIOLIB_ERR_NONE) {
        g_rivr_metrics.radio_tx_fail++;
        RIVR_LOGE(TAG, "transmit failed: %d", err);
        radio_start_rx();
        return false;
    }

    radio_start_rx();
    return true;
}

extern "C" void radio_service_rx(void)
{
    if (!s_dio1_pending) {
        return;
    }
    s_dio1_pending = false;

    size_t pkt_len = s_radio.getPacketLength(false);
    if (pkt_len == 0u || pkt_len > RF_MAX_PAYLOAD_LEN) {
        radio_start_rx();
        return;
    }

    rf_rx_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.len = (uint8_t)pkt_len;

    int err = s_radio.readData(frame.data, pkt_len);
    if (err != RADIOLIB_ERR_NONE) {
        RIVR_LOGW(TAG, "readData failed: %d", err);
        radio_start_rx();
        return;
    }

    frame.rssi_dbm   = (int16_t)s_radio.getRSSI();
    frame.snr_db     = (int8_t)s_radio.getSNR();
    frame.rx_mono_ms = tb_millis();
    s_last_rx_ms     = frame.rx_mono_ms;

    rb_try_push(&rf_rx_ringbuf, &frame);
    radio_start_rx();
}

extern "C" void radio_poll_rx(void)
{
    radio_service_rx();
}

extern "C" int16_t radio_get_rssi_inst(void)
{
    float rssi = -120.0f;
    if (!s_in_rx) return (int16_t)rssi;
    s_radio.getInstantRssi(&rssi);
    return (int16_t)rssi;
}

extern "C" void radio_check_timeouts(void)
{
    uint32_t now = tb_millis();

    if (s_last_rx_ms == 0u) {
        s_last_rx_ms = now;
        return;
    }

    if (!s_in_rx) return;

    if ((now - s_last_rx_ms) >= 60000u) {
        g_rivr_metrics.radio_rx_timeout++;
        RIVR_LOGW(TAG, "RX silent %" PRIu32 "ms – no reset (total=%" PRIu32 ")",
                  (uint32_t)(now - s_last_rx_ms),
                  g_rivr_metrics.radio_rx_timeout);
        s_last_rx_ms = now;
    }
}

extern "C" uint8_t radio_decode_frame(const rf_rx_frame_t *frame, char *out_buf, uint8_t out_len)
{
    if (!frame || frame->len < 3 || out_len == 0) return 0;

    const char *prefix = "DATA";
    switch (frame->data[0]) {
        case RF_FRAME_CHAT:   prefix = "CHAT";   break;
        case RF_FRAME_BEACON: prefix = "BEACON"; break;
        case RF_FRAME_ACK:    prefix = "ACK";    break;
        default:              prefix = "DATA";   break;
    }

    uint8_t payload_len = frame->len - 3u;
    int n = snprintf(out_buf, out_len, "%s:", prefix);
    if (n < 0 || n >= out_len) return 0;

    uint8_t remain = out_len - (uint8_t)n - 1u;
    uint8_t copy   = payload_len < remain ? payload_len : remain;
    memcpy(out_buf + n, frame->data + 3, copy);
    out_buf[n + copy] = '\0';
    return (uint8_t)(n + copy);
}

extern "C" uint16_t radio_frame_sender_tick(const rf_rx_frame_t *frame)
{
    if (!frame || frame->len < 3u) return 0u;
    return (uint16_t)(frame->data[1] | ((uint16_t)frame->data[2] << 8));
}

#endif /* RIVR_PLATFORM_NRF52840 && RIVR_RADIO_LR1110 */
