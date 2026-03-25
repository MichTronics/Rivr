/**
 * @file  platform_linux.c
 * @brief Raspberry Pi Linux platform HAL implementation.
 *
 * Provides the same platform_* API as platform_esp32.c and platform_nrf52.cpp
 * but maps to Linux kernel interfaces:
 *
 *   SPI  → /dev/spidev0.0 via SPI_IOC_MESSAGE ioctl (Linux spidev driver)
 *   GPIO → /dev/gpiochip0 via libgpiod v1 API
 *   DIO1 → Separate pthread blocking on gpiod_line_event_wait() (rising edge)
 *   ID   → Derived from the first available Ethernet MAC address
 *
 * Dependencies: libgpiod (package: libgpiod-dev), Linux kernel ≥ 4.8.
 *
 * Build:  see Makefile.linux  (-lgpiod)
 */

#define _GNU_SOURCE
#include "platform_linux.h"
#include "rivr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>

/* Satisfy the extern declared in platform_esp32.h (included transitively
 * by rivr_sources.c).  Not used on Linux — spidev fd is used instead. */
#include "../firmware_core/linux/include/driver/spi_master.h"
spi_device_handle_t g_spi_sx1262 = 0;

#define TAG "PLATFORM"

/* ── SPI file descriptor ─────────────────────────────────────────────────── */
static int s_spi_fd = -1;

/* ── GPIO chip and lines ─────────────────────────────────────────────────── */
static struct gpiod_chip *s_chip  = NULL;
static struct gpiod_line *s_nss   = NULL;  /* GPIO21 – chip select           */
static struct gpiod_line *s_reset = NULL;  /* GPIO18 – NRST                  */
static struct gpiod_line *s_rxen  = NULL;  /* GPIO12 – RF switch RX enable   */
static struct gpiod_line *s_txen  = NULL;  /* GPIO13 – RF switch TX enable   */
static struct gpiod_line *s_busy  = NULL;  /* GPIO20 – BUSY (input)          */
static struct gpiod_line *s_dio1  = NULL;  /* GPIO16 – DIO1 IRQ (input)      */

/* ── DIO1 interrupt thread ───────────────────────────────────────────────── */
static pthread_t   s_dio1_thread;
static void      (*s_dio1_callback)(void) = NULL;

static void *dio1_thread_fn(void *arg)
{
    (void)arg;

    struct gpiod_line_event event;

    while (1) {
        /* Block until rising edge on DIO1 or 1-second timeout. */
        struct timespec timeout = { .tv_sec = 1, .tv_nsec = 0 };
        int ret = gpiod_line_event_wait(s_dio1, &timeout);
        if (ret < 0) {
            /* Error (e.g., chip closed during shutdown) — exit thread. */
            break;
        }
        if (ret == 0) {
            /* Timeout — loop back to wait again. */
            continue;
        }
        /* Rising edge event available — read it to clear the event queue. */
        if (gpiod_line_event_read(s_dio1, &event) == 0) {
            if (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE) {
                if (s_dio1_callback) {
                    s_dio1_callback();
                }
            }
        }
    }
    return NULL;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static struct gpiod_line *open_output(unsigned int offset, int initial_val,
                                       const char *name)
{
    struct gpiod_line *line = gpiod_chip_get_line(s_chip, offset);
    if (!line) {
        fprintf(stderr, "[E][%s] gpiod_chip_get_line(%u) failed: %s\n",
                TAG, offset, strerror(errno));
        exit(1);
    }
    if (gpiod_line_request_output(line, "rivr", initial_val) < 0) {
        fprintf(stderr, "[E][%s] request_output GPIO%u (%s) failed: %s\n",
                TAG, offset, name, strerror(errno));
        exit(1);
    }
    return line;
}

static struct gpiod_line *open_input(unsigned int offset, const char *name)
{
    struct gpiod_line *line = gpiod_chip_get_line(s_chip, offset);
    if (!line) {
        fprintf(stderr, "[E][%s] gpiod_chip_get_line(%u) failed: %s\n",
                TAG, offset, strerror(errno));
        exit(1);
    }
    if (gpiod_line_request_input(line, "rivr") < 0) {
        fprintf(stderr, "[E][%s] request_input GPIO%u (%s) failed: %s\n",
                TAG, offset, name, strerror(errno));
        exit(1);
    }
    return line;
}

static struct gpiod_line *open_rising_edge(unsigned int offset, const char *name)
{
    struct gpiod_line *line = gpiod_chip_get_line(s_chip, offset);
    if (!line) {
        fprintf(stderr, "[E][%s] gpiod_chip_get_line(%u) failed: %s\n",
                TAG, offset, strerror(errno));
        exit(1);
    }
    /* Request rising-edge event monitoring. */
    if (gpiod_line_request_rising_edge_events(line, "rivr") < 0) {
        fprintf(stderr, "[E][%s] request_rising_edge GPIO%u (%s) failed: %s\n",
                TAG, offset, name, strerror(errno));
        exit(1);
    }
    return line;
}

/* ── platform_init ───────────────────────────────────────────────────────── */

void platform_init(void)
{
    RIVR_LOGI(TAG, "platform_init: opening SPI and GPIO (Linux/RPi)");

    /* ── spidev ── */
    s_spi_fd = open(RIVR_LINUX_SPI_DEV, O_RDWR);
    if (s_spi_fd < 0) {
        fprintf(stderr, "[E][%s] open(%s) failed: %s\n",
                TAG, RIVR_LINUX_SPI_DEV, strerror(errno));
        fprintf(stderr, "   Hint: enable SPI0 in /boot/config.txt and reboot.\n");
        exit(1);
    }

    /* SPI mode 0, 8-bit words, 8 MHz. */
    uint8_t  mode  = SPI_MODE_0;
    uint8_t  bits  = 8;
    uint32_t speed = SX1262_SPI_FREQ_HZ;

    if (ioctl(s_spi_fd, SPI_IOC_WR_MODE,          &mode)  < 0 ||
        ioctl(s_spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits)  < 0 ||
        ioctl(s_spi_fd, SPI_IOC_WR_MAX_SPEED_HZ,  &speed) < 0) {
        fprintf(stderr, "[E][%s] SPI ioctl failed: %s\n", TAG, strerror(errno));
        exit(1);
    }

    /* ── GPIO chip ── */
    s_chip = gpiod_chip_open(RIVR_LINUX_GPIO_CHIP);
    if (!s_chip) {
        fprintf(stderr, "[E][%s] gpiod_chip_open(%s) failed: %s\n",
                TAG, RIVR_LINUX_GPIO_CHIP, strerror(errno));
        exit(1);
    }

    /* ── Output lines (NSS high, RESET high, RXEN=0, TXEN=0 at init) ── */
    s_nss   = open_output(PIN_SX1262_NSS,   1, "NSS");
    s_reset = open_output(PIN_SX1262_RESET, 1, "RESET");
    s_rxen  = open_output(PIN_SX1262_RXEN,  0, "RXEN");
    s_txen  = open_output(PIN_SX1262_TXEN,  0, "TXEN");

    /* ── Input lines ── */
    s_busy  = open_input(PIN_SX1262_BUSY, "BUSY");

    /* ── DIO1: rising-edge event monitoring ── */
    s_dio1  = open_rising_edge(PIN_SX1262_DIO1, "DIO1");

    /* ── stdin non-blocking (for serial CLI) ── */
    {
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
        }
    }

    RIVR_LOGI(TAG, "platform_init: done"
              " (SPI=%s, chip=%s, NSS=GPIO%d, RESET=GPIO%d,"
              " RXEN=GPIO%d, TXEN=GPIO%d, BUSY=GPIO%d, DIO1=GPIO%d)",
              RIVR_LINUX_SPI_DEV, RIVR_LINUX_GPIO_CHIP,
              PIN_SX1262_NSS, PIN_SX1262_RESET,
              PIN_SX1262_RXEN, PIN_SX1262_TXEN,
              PIN_SX1262_BUSY, PIN_SX1262_DIO1);
}

/* ── SPI helpers ─────────────────────────────────────────────────────────── */

void platform_spi_cs_assert(void)
{
    gpiod_line_set_value(s_nss, 0);
}

void platform_spi_cs_release(void)
{
    gpiod_line_set_value(s_nss, 1);
}

void platform_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (!tx && !rx) return;

    /* Use a static zero buffer for dummy TX bytes when tx == NULL. */
    static uint8_t s_zeros[256];
    if (!tx) tx = s_zeros;

    /* Use a static discard buffer when rx == NULL. */
    static uint8_t s_discard[256];
    uint8_t *rx_p = rx ? rx : s_discard;

    /* spidev full-duplex transfer. */
    struct spi_ioc_transfer xfer = {
        .tx_buf        = (uintptr_t)tx,
        .rx_buf        = (uintptr_t)rx_p,
        .len           = len,
        .speed_hz      = SX1262_SPI_FREQ_HZ,
        .bits_per_word = 8,
        .cs_change     = 0,
    };

    platform_spi_cs_assert();
    if (ioctl(s_spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
        fprintf(stderr, "[E][%s] SPI_IOC_MESSAGE failed: %s\n",
                TAG, strerror(errno));
    }
    platform_spi_cs_release();
}

/* ── SX1262 helpers ─────────────────────────────────────────────────────── */

bool platform_sx1262_wait_busy(uint32_t timeout_ms)
{
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (gpiod_line_get_value(s_busy) == 1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t elapsed_ms = (uint32_t)(
            (now.tv_sec  - t0.tv_sec)  * 1000UL +
            (now.tv_nsec - t0.tv_nsec) / 1000000UL);
        if (elapsed_ms > timeout_ms) {
            fprintf(stderr, "[E][%s] SX1262 BUSY timeout (%u ms)\n",
                    TAG, timeout_ms);
            return false;
        }
        /* Brief yield to avoid burning 100% CPU in a tight poll. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000L };  /* 100 µs */
        nanosleep(&ts, NULL);
    }
    return true;
}

void platform_sx1262_reset(void)
{
    gpiod_line_set_value(s_reset, 0);
    /* Hold reset low for 1 ms. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
    nanosleep(&ts, NULL);
    gpiod_line_set_value(s_reset, 1);
    /* Wait at least 10 ms for the chip to come out of reset. */
    ts.tv_nsec = 10000000L;
    nanosleep(&ts, NULL);
}

void platform_sx1262_set_rxen(bool enable)
{
    gpiod_line_set_value(s_rxen, enable ? 1 : 0);
    gpiod_line_set_value(s_txen, enable ? 0 : 1);
}

/* ── DIO1 interrupt ──────────────────────────────────────────────────────── */

void platform_dio1_attach_isr(void (*isr)(void))
{
    s_dio1_callback = isr;

    int ret = pthread_create(&s_dio1_thread, NULL, dio1_thread_fn, NULL);
    if (ret != 0) {
        fprintf(stderr, "[E][%s] pthread_create for DIO1 failed: %s\n",
                TAG, strerror(ret));
        exit(1);
    }

    RIVR_LOGI(TAG, "platform: DIO1 interrupt thread started (GPIO%d)",
              PIN_SX1262_DIO1);
}

/* ── LED (no onboard LED — no-ops) ──────────────────────────────────────── */
void platform_led_on(void)     {}
void platform_led_off(void)    {}
void platform_led_toggle(void) {}

/* ── Delay ───────────────────────────────────────────────────────────────── */

void platform_delay_ms(uint32_t ms)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000L),
    };
    nanosleep(&ts, NULL);
}

/* ── Misc ─────────────────────────────────────────────────────────────────── */

void platform_restart(void) { exit(0); }

uint32_t platform_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}
