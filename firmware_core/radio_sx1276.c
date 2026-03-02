/**
 * @file  radio_sx1276.c
 * @brief SX1276/SX1278 LoRa radio driver.
 *
 * Implements the same public API as radio_sx1262.c so the rest of the
 * firmware is completely radio-agnostic.  Select this driver at build time
 * by setting RIVR_RADIO_SX1276=1 (CMakeLists.txt picks radio_sx1276.c
 * instead of radio_sx1262.c).
 *
 * TARGETED HARDWARE
 * ─────────────────
 *  LilyGo LoRa32 V2.1_1.6 with integrated SX1276 (868/915 MHz HF variant).
 *  Can also work with the 433 MHz SX1278 variant — just change RF_FREQ_HZ.
 *
 * KEY DIFFERENCES FROM SX1262 DRIVER
 * ────────────────────────────────────
 *  • All register accesses are plain SPI byte R/W (addr|0x80 = write,
 *    addr&0x7F = read); no command-opcode protocol like the SX1262.
 *  • No BUSY pin — mode transitions complete asynchronously; we use short
 *    vTaskDelay() guards after mode changes instead of polling BUSY.
 *  • Interrupt pin is DIO0 (GPIO26 on LilyGo), configured as RxDone.
 *    The variant maps it into the PIN_SX1262_DIO1 slot so no call-site
 *    change is needed in platform_esp32.c.
 *  • No external RF switch (LNA/PA selected internally by SX1276).
 *    platform_sx1262_set_rxen() is a no-op when RIVR_RFSWITCH_ENABLE=0.
 *  • RSSI formula (HF port, >779 MHz): RSSI_dBm = −157 + RegPktRssiValue
 *    (corrected for SNR < 0 per SX1276 §5.5.5).
 *
 * LoRa BW encoding (SX1276 DS §4.1.1.4):
 *   regval 0 = 7.8   1 = 10.4   2 = 15.6   3 = 20.8   4 = 31.25
 *          5 = 41.7  6 = 62.5   7 = 125    8 = 250    9 = 500  kHz
 *   → BW 125 kHz = 0x07  (SX1276 value, different from SX1262's 0x04)
 *
 * Coding-rate encoding (SX1276 RegModemConfig1 bits[3:1]):
 *   001 = 4/5   010 = 4/6   011 = 4/7   100 = 4/8  ← (CR=4/8 = 0b100)
 *
 * DETERMINISM INVARIANTS (same as SX1262 driver)
 * ─────────────────────────────────────────────────
 *  • radio_isr() sets ONLY s_dio1_pending.  No SPI, no RIVR, no alloc.
 *  • radio_transmit() is called only from the main-loop task.
 *  • All SPI access goes through platform_spi_transfer().
 */

#include "radio_sx1276.h"
#include "platform_esp32.h"
#include "timebase.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rivr_log.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

/* ── Compile-time SX1276 register encodings ─────────────────────────────── */
/* BW kHz → RegModemConfig1 bits[7:4]  (SX1276 DS §4.1.1.4)                */
#if   RF_BANDWIDTH_KHZ == 7
#  define _RF_BW_SX1276_BITS  0u
#elif RF_BANDWIDTH_KHZ == 10
#  define _RF_BW_SX1276_BITS  1u
#elif RF_BANDWIDTH_KHZ == 15
#  define _RF_BW_SX1276_BITS  2u
#elif RF_BANDWIDTH_KHZ == 20
#  define _RF_BW_SX1276_BITS  3u
#elif RF_BANDWIDTH_KHZ == 31
#  define _RF_BW_SX1276_BITS  4u
#elif RF_BANDWIDTH_KHZ == 41
#  define _RF_BW_SX1276_BITS  5u
#elif RF_BANDWIDTH_KHZ == 62
#  define _RF_BW_SX1276_BITS  6u
#elif RF_BANDWIDTH_KHZ == 125
#  define _RF_BW_SX1276_BITS  7u
#elif RF_BANDWIDTH_KHZ == 250
#  define _RF_BW_SX1276_BITS  8u
#elif RF_BANDWIDTH_KHZ == 500
#  define _RF_BW_SX1276_BITS  9u
#else
#  error "RF_BANDWIDTH_KHZ: unsupported — use 7/10/15/20/31/41/62/125/250/500"
#endif
/* CR denominator (4/N) → RegModemConfig1 bits[3:1]                         */
#if   RF_CODING_RATE == 5
#  define _RF_CR_SX1276_BITS  1u
#elif RF_CODING_RATE == 6
#  define _RF_CR_SX1276_BITS  2u
#elif RF_CODING_RATE == 7
#  define _RF_CR_SX1276_BITS  3u
#elif RF_CODING_RATE == 8
#  define _RF_CR_SX1276_BITS  4u
#else
#  error "RF_CODING_RATE: unsupported — use 5, 6, 7, or 8 (for CR 4/5..4/8)"
#endif
/* RegModemConfig1: [BW:4][CR:3][ImplicitHeader:1] — explicit header = 0    */
#define _SX1276_MODEM_CFG1  ((uint8_t)((_RF_BW_SX1276_BITS << 4) | (_RF_CR_SX1276_BITS << 1)))
/* RegModemConfig2: [SF:4][TxCont:1][CRCon:1][SymbTO_MSB:2] — CRC on       */
#define _SX1276_MODEM_CFG2  ((uint8_t)((RF_SPREADING_FACTOR << 4) | 0x04u))

#define TAG "RADIO"

/* ── SX1276 register map ─────────────────────────────────────────────────── */
#define REG_FIFO             0x00u  /* FIFO read / write access              */
#define REG_OP_MODE          0x01u  /* Operating mode + LoRa mode bit        */
#define REG_FR_MSB           0x06u  /* RF carrier frequency [23:16]          */
#define REG_FR_MID           0x07u  /* RF carrier frequency [15:8]           */
#define REG_FR_LSB           0x08u  /* RF carrier frequency [7:0]            */
#define REG_PA_CONFIG        0x09u  /* PA selection + output power           */
#define REG_PA_RAMP          0x0Au  /* PA ramp-up time                       */
#define REG_OCP              0x0Bu  /* Over-current protection               */
#define REG_LNA              0x0Cu  /* LNA gain                              */
#define REG_FIFO_ADDR_PTR    0x0Du  /* FIFO read/write pointer               */
#define REG_FIFO_TX_BASE     0x0Eu  /* TX FIFO base address                  */
#define REG_FIFO_RX_BASE     0x0Fu  /* RX FIFO base address                  */
#define REG_FIFO_RX_CUR_ADDR 0x10u  /* Start address of last RX packet       */
#define REG_IRQ_FLAGS_MASK   0x11u  /* 1 = mask (disable) IRQ bit            */
#define REG_IRQ_FLAGS        0x12u  /* IRQ status; write 1 to clear a bit    */
#define REG_RX_NB_BYTES      0x13u  /* Number of received payload bytes      */
#define REG_PKT_SNR          0x19u  /* Packet SNR — signed byte, /4 → dB     */
#define REG_PKT_RSSI         0x1Au  /* Packet RSSI raw value                 */
#define REG_RSSI             0x1Bu  /* Instantaneous RSSI raw value          */
#define REG_MODEM_CFG1       0x1Du  /* BW, CR, implicit-header flag          */
#define REG_MODEM_CFG2       0x1Eu  /* SF, TxContinuous, CRC-on, SymbTimeout */
#define REG_SYMB_TIMEOUT_LSB 0x1Fu  /* Symbol timeout LSBs                   */
#define REG_PREAMBLE_MSB     0x20u  /* Preamble length [15:8]                */
#define REG_PREAMBLE_LSB     0x21u  /* Preamble length [7:0]                 */
#define REG_PAYLOAD_LEN      0x22u  /* Payload length (TX) / actual len (RX) */
#define REG_MAX_PAYLOAD_LEN  0x23u  /* Maximum payload length                */
#define REG_MODEM_CFG3       0x26u  /* LDRO, AgcAutoOn                       */
#define REG_DIO_MAPPING1     0x40u  /* DIO0[7:6] DIO1[5:4] DIO2[3:2] DIO3[1:0] */
#define REG_VERSION          0x42u  /* Chip version — SX1276 = 0x12          */
#define REG_PA_DAC           0x4Du  /* 0x84 = default; 0x87 = enable +20 dBm */

/* ── Op-mode constants ───────────────────────────────────────────────────── */
#define LORA_MODE   0x80u           /* RegOpMode bit7 — enables LoRa mode    */
#define MODE_SLEEP  0x00u
#define MODE_STDBY  0x01u
#define MODE_TX     0x03u
#define MODE_RX_C   0x05u           /* continuous RX                         */

/* ── RegIrqFlags bit masks ───────────────────────────────────────────────── */
#define IRQ_RX_TIMEOUT      (1u << 7)
#define IRQ_RX_DONE         (1u << 6)
#define IRQ_PAYLOAD_CRC_ERR (1u << 5)
#define IRQ_VALID_HEADER    (1u << 4)
#define IRQ_TX_DONE         (1u << 3)

/* ── RegDioMapping1 values ───────────────────────────────────────────────── */
/* DIO0 bits[7:6]: 00=RxDone  01=TxDone  10=CadDone                         */
#define DIO0_RXDONE  0x00u
#define DIO0_TXDONE  0x40u

/* ── PA configuration ────────────────────────────────────────────────────── */
/* LilyGo LoRa32 wires the SX1276 PA_BOOST pin (not RFO); PaSelect=1.       */
/* RegPaConfig: bit7=PaSelect, bits[6:4]=MaxPower, bits[3:0]=OutputPower     */
/*   PA_BOOST power: Pout = 2 + OutputPower [dBm]  (with PaDac=0x84)        */
/*   PA_BOOST +20 dBm mode: PaDac=0x87, OutputPower must be 15 (0x0F)       */
#define PA_CFG_BOOST_MAX  0x8Fu    /* PaSelect=1, MaxPower=7, OutputPower=15 */
#define PA_DAC_20DBM      0x87u    /* Enable +20 dBm overload (needs 150 mA) */
#define PA_DAC_DEFAULT    0x84u    /* Standard PA_BOOST (+17 dBm max)        */

/* ── Ring-buffer storage (static, no heap) ───────────────────────────────── */
static rf_rx_frame_t   s_rx_storage[RF_RX_RINGBUF_CAP];
static rf_tx_request_t s_tx_storage[RF_TX_QUEUE_CAP];

/** True while the radio is in continuous-RX mode. */
static volatile bool s_in_rx = false;

/** Set by radio_isr() when DIO0 raises a RxDone rising edge. */
static volatile bool s_dio1_pending = false;

rb_t rf_rx_ringbuf;
rb_t rf_tx_queue;

/* ── Low-level SPI register helpers ─────────────────────────────────────── */

/**
 * Write one byte to SX1276 register @addr.
 * SPI protocol: [addr|0x80, value] — platform_spi_transfer handles CS.
 */
static void sx1276_write_reg(uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(addr | 0x80u), val };
    platform_spi_transfer(tx, NULL, 2);
}

/**
 * Read one byte from SX1276 register @addr.
 * SPI protocol: [addr&0x7F, 0x00] → rx[1] = register value.
 */
static uint8_t sx1276_read_reg(uint8_t addr)
{
    uint8_t tx[2] = { (uint8_t)(addr & 0x7Fu), 0x00u };
    uint8_t rx[2] = { 0x00u, 0x00u };
    platform_spi_transfer(tx, rx, 2);
    return rx[1];
}

/**
 * Write @len bytes from @data into the SX1276 FIFO (burst write to RegFifo).
 * FIFO pointer must be configured by caller before calling this.
 */
static void sx1276_write_fifo(const uint8_t *data, uint8_t len)
{
    uint8_t tx_buf[1u + RF_MAX_PAYLOAD_LEN];
    tx_buf[0] = (uint8_t)(REG_FIFO | 0x80u);   /* write access */
    memcpy(tx_buf + 1, data, len);
    platform_spi_transfer(tx_buf, NULL, (uint16_t)(1u + len));
}

/**
 * Burst-read @len bytes from the SX1276 FIFO into @data.
 * FIFO pointer must be configured by caller before calling this.
 */
static void sx1276_read_fifo(uint8_t *data, uint8_t len)
{
    uint8_t tx_buf[1u + RF_MAX_PAYLOAD_LEN];
    uint8_t rx_buf[1u + RF_MAX_PAYLOAD_LEN];
    memset(tx_buf, 0x00, (size_t)(1u + len));
    tx_buf[0] = (uint8_t)(REG_FIFO & 0x7Fu);   /* read access */
    platform_spi_transfer(tx_buf, rx_buf, (uint16_t)(1u + len));
    memcpy(data, rx_buf + 1, len);
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

void radio_init_buffers_only(void)
{
    /* Safe to call before platform_init() (simulation builds). */
    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));
    RIVR_LOGI(TAG, "radio_init_buffers_only: ringbufs ready (SIM MODE)");
}

void radio_init(void)
{
    RIVR_LOGI(TAG, "radio_init: SX1276 reset + configure");

    rb_init(&rf_rx_ringbuf, s_rx_storage, RF_RX_RINGBUF_CAP, sizeof(rf_rx_frame_t));
    rb_init(&rf_tx_queue,   s_tx_storage, RF_TX_QUEUE_CAP,   sizeof(rf_tx_request_t));

    /* Hard reset via platform (toggles PIN_SX1262_RESET low for 1 ms). */
    platform_sx1262_reset();
    /* SX1276 requires ≥5 ms after NRESET high before SPI is usable. */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ── Enter LoRa mode ─────────────────────────────────────────────────
     * The LongRangeMode bit can only be written while the chip is in SLEEP.
     * Sequence: SLEEP → set LORA_MODE → STANDBY.
     */
    sx1276_write_reg(REG_OP_MODE, LORA_MODE | MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    sx1276_write_reg(REG_OP_MODE, LORA_MODE | MODE_STDBY);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Verify chip version (SX1276 = 0x12; SX1278 also = 0x12). */
    uint8_t ver = sx1276_read_reg(REG_VERSION);
    RIVR_LOGI(TAG, "SX1276 version register = 0x%02x (expected 0x12)", ver);
    if (ver != 0x12) {
        ESP_LOGW(TAG, "Unexpected SX127x version — continuing anyway");
    }

    /* ── RF frequency ────────────────────────────────────────────────────
     * Frf = freq_hz × 2^19 / Fosc   (Fosc = 32 MHz)
     *     = freq_hz × 524288 / 32000000
     * Integer calculation avoids float rounding errors.
     */
    {
        uint32_t frf = (uint32_t)(((uint64_t)RF_FREQ_HZ * 524288ULL) / 32000000ULL);
        sx1276_write_reg(REG_FR_MSB, (uint8_t)(frf >> 16));
        sx1276_write_reg(REG_FR_MID, (uint8_t)(frf >>  8));
        sx1276_write_reg(REG_FR_LSB, (uint8_t)(frf));
    }

    /* ── FIFO base addresses (TX and RX both start at 0x00) ───────────── */
    sx1276_write_reg(REG_FIFO_TX_BASE, 0x00);
    sx1276_write_reg(REG_FIFO_RX_BASE, 0x00);

    /* ── PA configuration ────────────────────────────────────────────────
     * LilyGo LoRa32 V2.1_1.6 connects the SX1276 to the PA_BOOST pin.
     * PaSelect=1 (bit7), MaxPower=7 (bits[6:4]=111), OutputPower=15 (0x0F)
     * → with PA_DAC=0x87 this gives +20 dBm.
     * Without +20 dBm PaDac setting max is +17 dBm.
     */
    /* PA configuration — RF_TX_POWER_DBM sets the target output power.
     * SX1276 PA_BOOST pin (LilyGo wiring): Pout = 2 + OutputPower [dBm].
     * +20 dBm mode (PaDac=0x87): OutputPower must be 15; needs 150–240 mA.
     * Standard mode (PaDac=0x84): max +17 dBm.                            */
#if RF_TX_POWER_DBM > 17
    sx1276_write_reg(REG_PA_CONFIG, 0x8Fu);   /* PA_BOOST, OutputPower=15  */
    sx1276_write_reg(REG_PA_DAC,    0x87u);   /* enable +20 dBm mode       */
    sx1276_write_reg(REG_OCP, 0x3Bu);         /* OCP: 240 mA               */
#else
    sx1276_write_reg(REG_PA_CONFIG, (uint8_t)(0x80u | ((uint8_t)(RF_TX_POWER_DBM) - 2u)));
    sx1276_write_reg(REG_PA_DAC,    0x84u);   /* standard PA_BOOST         */
    sx1276_write_reg(REG_OCP, 0x2Bu);         /* OCP: ~100 mA              */
#endif

    /* LNA: maximum gain (3-dB steps), LNA boost on for HF port. */
    sx1276_write_reg(REG_LNA, 0x23u);   /* LnaGain=0b001(max), LnaBoostHf=0b11 */

    /* ── LoRa modem configuration ────────────────────────────────────────
     *
     * RegModemConfig1 (0x1D)
     *   bits[7:4] = Bw          0111 = 125 kHz
     *   bits[3:1] = CodingRate  100  = 4/8
     *   bit [0]   = ImplicitHeaderModeOn  0 = explicit header
     *   → computed at compile time as _SX1276_MODEM_CFG1
     *
     * RegModemConfig2 (0x1E)
     *   bits[7:4] = SpreadingFactor  (RF_SPREADING_FACTOR)
     *   bit [3]   = TxContinuousMode 0
     *   bit [2]   = RxPayloadCrcOn   1    ← drop frames with bad CRC
     *   bits[1:0] = SymbTimeout MSBs 00
     *   → computed at compile time as _SX1276_MODEM_CFG2
     *
     * RegModemConfig3 (0x26)
     *   bit[3] = LowDataRateOptimize  0 (not needed for SF8/BW125)
     *   bit[2] = AgcAutoOn            1 (automatic gain control)
     *   → 0x04
     */
    sx1276_write_reg(REG_MODEM_CFG1, _SX1276_MODEM_CFG1);
    sx1276_write_reg(REG_MODEM_CFG2, _SX1276_MODEM_CFG2);
    sx1276_write_reg(REG_MODEM_CFG3, 0x04u);

    /* Symbol timeout: 0x3FF = 1023 symbols (generous window for SF8). */
    sx1276_write_reg(REG_MODEM_CFG2,   /* update SymbTimeout MSBs = 0b11 */
        (sx1276_read_reg(REG_MODEM_CFG2) & 0xFCu) | 0x03u);
    sx1276_write_reg(REG_SYMB_TIMEOUT_LSB, 0xFFu);

    /* Preamble length (8 symbols). */
    sx1276_write_reg(REG_PREAMBLE_MSB, 0x00u);
    sx1276_write_reg(REG_PREAMBLE_LSB, RF_PREAMBLE_LEN);

    /* Maximum payload length = 255 bytes. */
    sx1276_write_reg(REG_MAX_PAYLOAD_LEN, (uint8_t)RF_MAX_PAYLOAD_LEN);

    /* ── DIO0 mapping — RxDone ───────────────────────────────────────────
     * RegDioMapping1: DIO0[7:6]=00 → RxDone during RX (changed to 01 for TX).
     * DIO1[5:4]=00 → RxTimeout (not used — no ISR on DIO1).
     */
    sx1276_write_reg(REG_DIO_MAPPING1, DIO0_RXDONE);

    /* Unmask RxDone and TxDone; mask everything else. */
    sx1276_write_reg(REG_IRQ_FLAGS_MASK,
        (uint8_t)~(IRQ_RX_DONE | IRQ_TX_DONE));  /* 1=masked, 0=enabled */

    /* Clear all pending IRQ flags. */
    sx1276_write_reg(REG_IRQ_FLAGS, 0xFFu);

    /* ── DIO0 ISR ────────────────────────────────────────────────────────
     * Attach to PIN_SX1262_DIO1 — the LilyGo variant maps GPIO26 (= DIO0
     * on the physical module) into this slot.  Rising edge = RxDone fired.
     */
    gpio_isr_handler_add(PIN_SX1262_DIO1, radio_isr, NULL);

    RIVR_LOGI(TAG, "radio_init: done  SF%u BW%u kHz  freq %lu Hz",
             (unsigned)RF_SPREADING_FACTOR,
             (unsigned)RF_BANDWIDTH_KHZ,
             (unsigned long)RF_FREQ_HZ);
}

/* ── RX mode ─────────────────────────────────────────────────────────────── */

void radio_start_rx(void)
{
    /* Reset FIFO pointer to RX base before entering RX mode. */
    sx1276_write_reg(REG_FIFO_ADDR_PTR, sx1276_read_reg(REG_FIFO_RX_BASE));

    /* DIO0 = RxDone for continuous RX. */
    sx1276_write_reg(REG_DIO_MAPPING1, DIO0_RXDONE);

    /* Clear any stale IRQ flags. */
    sx1276_write_reg(REG_IRQ_FLAGS, 0xFFu);

    /* Enter continuous RX mode. */
    sx1276_write_reg(REG_OP_MODE, LORA_MODE | MODE_RX_C);
    s_in_rx = true;
    RIVR_LOGI(TAG, "RX mode started");
}

/* ── ISR ─────────────────────────────────────────────────────────────────── *
 *
 * Identical pattern to the SX1262 driver: only sets a flag, NO SPI calls.
 * spi_device_transmit() takes a FreeRTOS semaphore and must not be called
 * from ISR context — doing so triggers the Interrupt WDT.
 *
 * Called on the rising edge of DIO0 (= PIN_SX1262_DIO1 per variant config).
 */
void IRAM_ATTR radio_isr(void *arg)
{
    (void)arg;
    s_dio1_pending = true;
}

/* ── Deferred RX service (called each main-loop iteration) ─────────────── */

void radio_service_rx(void)
{
    if (!s_dio1_pending) return;
    s_dio1_pending = false;

    /* 1. Read and clear IRQ flags. */
    uint8_t irq = sx1276_read_reg(REG_IRQ_FLAGS);
    sx1276_write_reg(REG_IRQ_FLAGS, irq);   /* write-1-to-clear */

    if (!(irq & IRQ_RX_DONE)) return;       /* spurious edge */

    if (irq & IRQ_PAYLOAD_CRC_ERR) {
        ESP_LOGW(TAG, "RX CRC error — frame discarded");
        return;
    }

    /* 2. Retrieve received payload length. */
    uint8_t payload_len = sx1276_read_reg(REG_RX_NB_BYTES);
    if (payload_len == 0) return;   /* SX1276: uint8_t so > 255 is impossible */

    /* 3. Point FIFO address pointer at the start of the received packet. */
    uint8_t fifo_rx_addr = sx1276_read_reg(REG_FIFO_RX_CUR_ADDR);
    sx1276_write_reg(REG_FIFO_ADDR_PTR, fifo_rx_addr);

    /* 4. Read signal quality.
     *    SNR: signed byte, value/4 gives dB.
     *    RSSI (HF port, >779 MHz): -157 + RegPktRssiValue [dBm]
     *    If SNR < 0, the datasheet correction applies:
     *      RSSI_pkt = -157 + RegPktRssiValue + SNR/4
     */
    int8_t  snr_raw  = (int8_t)sx1276_read_reg(REG_PKT_SNR);
    uint8_t rssi_raw = sx1276_read_reg(REG_PKT_RSSI);
    int8_t  snr_db   = snr_raw / 4;

    int16_t pkt_rssi_dbm;
    if (snr_db < 0) {
        pkt_rssi_dbm = -157 + (int16_t)rssi_raw + (int16_t)snr_db;
    } else {
        pkt_rssi_dbm = -157 + (int16_t)rssi_raw;
    }

    /* 5. Burst-read payload from FIFO. */
    rf_rx_frame_t frame;
    memset(&frame, 0, sizeof(frame));   /* from_id = 0 (unknown on real HW) */
    sx1276_read_fifo(frame.data, payload_len);

    frame.len        = payload_len;
    frame.rssi_dbm   = pkt_rssi_dbm;
    frame.snr_db     = snr_db;
    frame.rx_mono_ms = (uint32_t)atomic_load_explicit(&g_mono_ms, memory_order_relaxed);

    /* Push to ring-buffer (drops silently if full). */
    rb_try_push(&rf_rx_ringbuf, &frame);
}

/* ── TX ──────────────────────────────────────────────────────────────────── */

bool radio_transmit(const rf_tx_request_t *req)
{
    if (!req || req->len == 0) return false;

    /* ── 1. Enter standby to stop any ongoing RX ──────────────────────── */
    sx1276_write_reg(REG_OP_MODE, LORA_MODE | MODE_STDBY);
    vTaskDelay(pdMS_TO_TICKS(1));
    s_in_rx = false;

    /* ── 2. Reconfigure DIO0 → TxDone during TX ──────────────────────── */
    sx1276_write_reg(REG_DIO_MAPPING1, DIO0_TXDONE);

    /* ── 3. Write payload into FIFO ──────────────────────────────────── */
    sx1276_write_reg(REG_FIFO_ADDR_PTR, 0x00u);   /* TX base = 0x00 */
    sx1276_write_fifo(req->data, req->len);
    sx1276_write_reg(REG_PAYLOAD_LEN, req->len);

    /* ── 4. Clear all IRQ flags before starting TX ─────────────────── */
    sx1276_write_reg(REG_IRQ_FLAGS, 0xFFu);

    /* ── 5. Start TX ──────────────────────────────────────────────────── */
    sx1276_write_reg(REG_OP_MODE, LORA_MODE | MODE_TX);

    /* ── 6. Poll TxDone (interrupt flag, no DIO0 needed) ─────────────── *
     * Deadline = 2 × ToA + 100 ms safety margin (same formula as sx1262).
     */
    uint32_t deadline_ms = tb_millis() + req->toa_us / 1000u * 2u + 100u;
    while (tb_millis() < deadline_ms) {
        uint8_t flags = sx1276_read_reg(REG_IRQ_FLAGS);
        if (flags & IRQ_TX_DONE) {
            sx1276_write_reg(REG_IRQ_FLAGS, 0xFFu);   /* clear */
            sx1276_write_reg(REG_DIO_MAPPING1, DIO0_RXDONE);
            radio_start_rx();
            return true;
        }
    }

    /* ── 7. Timeout ──────────────────────────────────────────────────── */
    ESP_LOGE(TAG, "TX deadline exceeded");
    sx1276_write_reg(REG_IRQ_FLAGS, 0xFFu);
    sx1276_write_reg(REG_DIO_MAPPING1, DIO0_RXDONE);
    radio_start_rx();
    return false;
}

/* ── Frame decoder helpers ───────────────────────────────────────────────── */

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
    /* Polling-mode stub — ISR-driven path used in all real builds. */
}

int16_t radio_get_rssi_inst(void)
{
    if (!s_in_rx) return -120;
    /* HF port: RSSI_dBm = -157 + RegRssiValue (instantaneous, not packet) */
    return -157 + (int16_t)sx1276_read_reg(REG_RSSI);
}

/**
 * @brief Timeout/recovery check — SX1276 stub.
 *
 * Full recovery logic (RX-silence watchdog, BUSY-stuck detection) is
 * implemented in radio_sx1262.c.  The SX1276 driver does not yet have
 * a recovery path; this no-op satisfies the main.c call site so the
 * lilygo_lora32_v21 build links correctly.
 */
void radio_check_timeouts(void)
{
    /* No-op: SX1276 driver does not implement timeout recovery yet. */
}
