/**
 * @file  display/display.c
 * @brief Non-blocking SSD1306 128×64 I2C OLED driver — 5-page rotating status.
 *
 * ARCHITECTURE
 * ────────────
 *  • Static 1024-byte framebuffer (128 columns × 8 pages, 1 bit/pixel).
 *  • Classic 5×8 pixel ASCII font (characters 0x20–0x7E, ~480 bytes flash).
 *  • Each text row = one SSD1306 page (8 px tall).  21 chars/row, 8 rows.
 *  • Flush sends 8 × 133-byte I2C transactions (page-addressed mode).
 *    At 400 kHz this takes ≈ 25 ms; happens at most every 200 ms.
 *  • ISR (DIO1) fires unconditionally during I2C flush; radio ring-buffer
 *    is filled normally.  No radio frames are lost.
 *
 * COMPILE GUARD
 * ─────────────
 *  This file compiles to nothing when FEATURE_DISPLAY != 1.
 *  Add -DFEATURE_DISPLAY=1 to build_flags in platformio.ini to enable.
 */

#include "display.h"

#if FEATURE_DISPLAY   /* ═══════════════ real implementation ═════════════════ */

#include <string.h>
#include <stdio.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "../timebase.h"

#define TAG "DISPLAY"

/* ── Config ──────────────────────────────────────────────────────────────── */

#define I2C_BUS_PORT       I2C_NUM_0
#define I2C_BUS_SPEED_HZ   100000u   /**< 100 kHz — reliable with all SSD1306 clones */
#define I2C_TIMEOUT_MS     20        /**< Per-transaction timeout ms; keep short to     *
                                      * avoid blocking main task > WDT period.          *
                                      * At 100 kHz a 3-byte tx ≈ 0.3 ms; 20 ms is     *
                                      * already very generous.                          */
#define SELFTEST_MS        400       /**< All-pixels-ON test duration at boot           */
#define FB_COLS            128u
#define FB_PAGES           8u               /**< SSD1306 row pages            */
#define FB_BYTES           (FB_COLS * FB_PAGES)  /**< 1024                   */
#define FONT_FIRST         0x20u            /**< First printable ASCII char   */
#define FONT_LAST          0x7Eu            /**< Last printable ASCII char    */
#define CHAR_W             5u               /**< Glyph pixel width            */
#define CHAR_STRIDE        6u               /**< Pixels per char (5 + 1 gap)  */
#define CHARS_PER_ROW      (FB_COLS / CHAR_STRIDE)  /**< 21                  */

/* ── 5×8 ASCII font (characters 0x20–0x7E, 5 bytes each, public domain) ─── *
 *
 * Each byte is one vertical column of 8 pixels, MSB = bottom.
 * Index 0 in FONT5X8 = space (0x20).
 * ─────────────────────────────────────────────────────────────────────────── */
static const uint8_t FONT5X8[][CHAR_W] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 0x20  */  {0x00,0x00,0x5F,0x00,0x00}, /* !  */
    {0x00,0x07,0x00,0x07,0x00}, /* "  */  {0x14,0x7F,0x14,0x7F,0x14}, /* #  */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $  */  {0x23,0x13,0x08,0x64,0x62}, /* %  */
    {0x36,0x49,0x55,0x22,0x50}, /* &  */  {0x00,0x05,0x03,0x00,0x00}, /* '  */
    {0x00,0x1C,0x22,0x41,0x00}, /* (  */  {0x00,0x41,0x22,0x1C,0x00}, /* )  */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* *  */  {0x08,0x08,0x3E,0x08,0x08}, /* +  */
    {0x00,0x50,0x30,0x00,0x00}, /* ,  */  {0x08,0x08,0x08,0x08,0x08}, /* -  */
    {0x00,0x60,0x60,0x00,0x00}, /* .  */  {0x20,0x10,0x08,0x04,0x02}, /* /  */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0  */  {0x00,0x42,0x7F,0x40,0x00}, /* 1  */
    {0x42,0x61,0x51,0x49,0x46}, /* 2  */  {0x21,0x41,0x45,0x4B,0x31}, /* 3  */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4  */  {0x27,0x45,0x45,0x45,0x39}, /* 5  */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6  */  {0x01,0x71,0x09,0x05,0x03}, /* 7  */
    {0x36,0x49,0x49,0x49,0x36}, /* 8  */  {0x06,0x49,0x49,0x29,0x1E}, /* 9  */
    {0x00,0x36,0x36,0x00,0x00}, /* :  */  {0x00,0x56,0x36,0x00,0x00}, /* ;  */
    {0x00,0x08,0x14,0x22,0x41}, /* <  */  {0x14,0x14,0x14,0x14,0x14}, /* =  */
    {0x41,0x22,0x14,0x08,0x00}, /* >  */  {0x02,0x01,0x51,0x09,0x06}, /* ?  */
    {0x32,0x49,0x79,0x41,0x3E}, /* @  */  {0x7E,0x11,0x11,0x11,0x7E}, /* A  */
    {0x7F,0x49,0x49,0x49,0x36}, /* B  */  {0x3E,0x41,0x41,0x41,0x22}, /* C  */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D  */  {0x7F,0x49,0x49,0x49,0x41}, /* E  */
    {0x7F,0x09,0x09,0x09,0x01}, /* F  */  {0x3E,0x41,0x49,0x49,0x7A}, /* G  */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H  */  {0x00,0x41,0x7F,0x41,0x00}, /* I  */
    {0x20,0x40,0x41,0x3F,0x01}, /* J  */  {0x7F,0x08,0x14,0x22,0x41}, /* K  */
    {0x7F,0x40,0x40,0x40,0x40}, /* L  */  {0x7F,0x02,0x04,0x02,0x7F}, /* M  */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N  */  {0x3E,0x41,0x41,0x41,0x3E}, /* O  */
    {0x7F,0x09,0x09,0x09,0x06}, /* P  */  {0x3E,0x41,0x51,0x21,0x5E}, /* Q  */
    {0x7F,0x09,0x19,0x29,0x46}, /* R  */  {0x46,0x49,0x49,0x49,0x31}, /* S  */
    {0x01,0x01,0x7F,0x01,0x01}, /* T  */  {0x3F,0x40,0x40,0x40,0x3F}, /* U  */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V  */  {0x3F,0x40,0x38,0x40,0x3F}, /* W  */
    {0x63,0x14,0x08,0x14,0x63}, /* X  */  {0x07,0x08,0x70,0x08,0x07}, /* Y  */
    {0x61,0x51,0x49,0x45,0x43}, /* Z  */  {0x00,0x7F,0x41,0x41,0x00}, /* [  */
    {0x02,0x04,0x08,0x10,0x20}, /* \  */  {0x00,0x41,0x41,0x7F,0x00}, /* ]  */
    {0x04,0x02,0x01,0x02,0x04}, /* ^  */  {0x40,0x40,0x40,0x40,0x40}, /* _  */
    {0x00,0x01,0x02,0x04,0x00}, /* `  */  {0x20,0x54,0x54,0x54,0x78}, /* a  */
    {0x7F,0x48,0x44,0x44,0x38}, /* b  */  {0x38,0x44,0x44,0x44,0x20}, /* c  */
    {0x38,0x44,0x44,0x48,0x7F}, /* d  */  {0x38,0x54,0x54,0x54,0x18}, /* e  */
    {0x08,0x7E,0x09,0x01,0x02}, /* f  */  {0x08,0x14,0x54,0x54,0x3C}, /* g  */
    {0x7F,0x08,0x04,0x04,0x78}, /* h  */  {0x00,0x44,0x7D,0x40,0x00}, /* i  */
    {0x20,0x40,0x44,0x3D,0x00}, /* j  */  {0x7F,0x10,0x28,0x44,0x00}, /* k  */
    {0x00,0x41,0x7F,0x40,0x00}, /* l  */  {0x7C,0x04,0x18,0x04,0x78}, /* m  */
    {0x7C,0x08,0x04,0x04,0x78}, /* n  */  {0x38,0x44,0x44,0x44,0x38}, /* o  */
    {0x7C,0x14,0x14,0x14,0x08}, /* p  */  {0x08,0x14,0x14,0x18,0x7C}, /* q  */
    {0x7C,0x08,0x04,0x04,0x08}, /* r  */  {0x48,0x54,0x54,0x54,0x20}, /* s  */
    {0x04,0x3F,0x44,0x40,0x20}, /* t  */  {0x3C,0x40,0x40,0x40,0x7C}, /* u  */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v  */  {0x3C,0x40,0x20,0x40,0x3C}, /* w  */
    {0x44,0x28,0x10,0x28,0x44}, /* x  */  {0x0C,0x50,0x50,0x50,0x3C}, /* y  */
    {0x44,0x64,0x54,0x4C,0x44}, /* z  */  {0x00,0x08,0x36,0x41,0x00}, /* {  */
    {0x00,0x00,0x7F,0x00,0x00}, /* |  */  {0x00,0x41,0x36,0x08,0x00}, /* }  */
    {0x08,0x08,0x2A,0x1C,0x08},           /* ~  */
};

/* ── Static state ────────────────────────────────────────────────────────── */

static uint8_t               s_fb[FB_BYTES];       /**< Pixel framebuffer (1024 B)     */
/* Flush buffer: 0x40 control + 1024 bytes GDDRAM; static to avoid stack overflow */
static uint8_t               s_flush_buf[1u + FB_BYTES];
static i2c_master_bus_handle_t s_bus;              /**< I2C bus handle                 */
static i2c_master_dev_handle_t s_dev;              /**< SSD1306 device handle          */
static uint8_t               s_page;          /**< Current display page 0–4   */
static uint8_t               s_flush_fail_cnt; /**< Consecutive flush failures  */
static uint32_t              s_last_update_ms;/**< Last full refresh timestamp */
static uint32_t              s_last_rotate_ms;/**< Last page-rotation timestamp*/
static bool                  s_ok;            /**< true after successful init  */

/* ── SSD1306 low-level helpers ──────────────────────────────────────────── *
 *
 * Each helper sends a single I2C transaction.  The SSD1306 I2C protocol:
 *   Byte 0 (control): 0x00 = command stream, 0x40 = data stream
 *   Byte 1+: command / data bytes
 * Sending one command per transaction is the most compatible approach.
 * ─────────────────────────────────────────────────────────────────────────── */

/** Send a single 1-byte command (e.g. 0xAE, 0xAF). */
static esp_err_t ssd1306_cmd1(uint8_t c)
{
    uint8_t b[2] = {0x00u, c};
    return i2c_master_transmit(s_dev, b, sizeof(b), I2C_TIMEOUT_MS);
}

/** Send a 2-byte command sequence (cmd + 1 data byte). */
static esp_err_t ssd1306_cmd2(uint8_t c, uint8_t d)
{
    uint8_t b[3] = {0x00u, c, d};
    return i2c_master_transmit(s_dev, b, sizeof(b), I2C_TIMEOUT_MS);
}

/** Send a 3-byte command sequence (cmd + 2 data bytes). */
static esp_err_t ssd1306_cmd3(uint8_t c, uint8_t d0, uint8_t d1)
{
    uint8_t b[4] = {0x00u, c, d0, d1};
    return i2c_master_transmit(s_dev, b, sizeof(b), I2C_TIMEOUT_MS);
}

/* ── SSD1306 init command sequence ──────────────────────────────────────── */

static esp_err_t ssd1306_hw_init(void)
{
    esp_err_t r;
    /* Each call is a separate I2C transaction — reliable on all clones */
    if ((r = ssd1306_cmd1(0xAEu)) != ESP_OK) return r; /* display off          */
    ssd1306_cmd2(0xD5u, 0x80u);   /* clock divide / osc freq                  */
    ssd1306_cmd2(0xA8u, 0x3Fu);   /* mux ratio = 63 (64 rows)                 */
    ssd1306_cmd2(0xD3u, 0x00u);   /* display offset = 0                       */
    ssd1306_cmd1(0x40u);          /* start line = 0                           */
    ssd1306_cmd2(0x8Du, 0x14u);   /* charge pump: enable                      */
    ssd1306_cmd2(0x20u, 0x02u);   /* page addressing mode (most compatible)   */
    ssd1306_cmd1(0xA1u);          /* segment remap: col127→SEG0              */
    ssd1306_cmd1(0xC8u);          /* COM scan: descending                     */
    ssd1306_cmd2(0xDAu, 0x12u);   /* COM pins: alt, no remap (128×64)         */
    ssd1306_cmd2(0x81u, 0xFFu);   /* contrast: maximum                        */
    ssd1306_cmd2(0xD9u, 0xF1u);   /* pre-charge period                        */
    ssd1306_cmd2(0xDBu, 0x20u);   /* VCOMH deselect level (0.77×VCC)         */
    ssd1306_cmd1(0xA4u);          /* output follows RAM                       */
    ssd1306_cmd1(0xA6u);          /* normal display (not inverted)            */
    r = ssd1306_cmd1(0xAFu);      /* display ON                               */
    return r;
}

/* ── Private helpers ─────────────────────────────────────────────────────── */

/** Write a single character glyph at text column @p col, text row @p row. */
static void fb_char(uint8_t col, uint8_t row, char c)
{
    if ((uint8_t)c < FONT_FIRST || (uint8_t)c > FONT_LAST) c = ' ';
    const uint8_t *g = FONT5X8[(uint8_t)c - FONT_FIRST];
    uint16_t px = (uint16_t)col * CHAR_STRIDE;
    if (px + CHAR_W > FB_COLS) return;
    uint16_t base = (uint16_t)row * FB_COLS;
    for (uint8_t i = 0u; i < CHAR_W; i++) {
        s_fb[base + px + i] = g[i];
    }
    s_fb[base + px + CHAR_W] = 0x00u;  /* 1-pixel gap after glyph */
}

/** Render a NUL-terminated string starting at text column @p col, row @p row.
 *  Characters past column 20 are silently truncated. */
static void fb_str(uint8_t col, uint8_t row, const char *s)
{
    while (s && *s && col < CHARS_PER_ROW) {
        fb_char(col++, row, *s++);
    }
}

/** Render a string right-aligned so its last character lands at column 20. */
static void fb_str_right(uint8_t row, const char *s)
{
    if (!s) return;
    size_t len = strnlen(s, CHARS_PER_ROW);
    uint8_t col = (CHARS_PER_ROW >= (uint8_t)len)
                     ? (uint8_t)(CHARS_PER_ROW - len) : 0u;
    fb_str(col, row, s);
}

/** Draw a full-width horizontal rule (solid line) on page @p row. */
static void fb_hline(uint8_t row)
{
    memset(s_fb + (uint16_t)row * FB_COLS, 0x01u, FB_COLS);
}

/** Clear the entire framebuffer to black. */
static void fb_clear(void)
{
    memset(s_fb, 0x00u, sizeof(s_fb));
}

/** Flush the complete framebuffer to the SSD1306.
 *
 *  Uses page-addressing mode (set in init via 0x20, 0x02).
 *  For each of the 8 pages:
 *    1. B0h|page  — set page start address
 *    2. 0x00      — set column lower nibble = 0
 *    3. 0x10      — set column upper nibble = 0
 *    4. 0x40 + 128 bytes of pixel data      (129 bytes in one transfer)
 *  8 × 129-byte writes — no single large burst needed.
 */
static void ssd1306_flush(void)
{
    esp_err_t err;
    for (uint8_t pg = 0u; pg < FB_PAGES; pg++) {
        /* Commands: set page, column-low=0, column-high=0 */
        err = ssd1306_cmd1((uint8_t)(0xB0u | pg));
        if (err != ESP_OK) goto flush_err;
        err = ssd1306_cmd1(0x00u);
        if (err != ESP_OK) goto flush_err;
        err = ssd1306_cmd1(0x10u);
        if (err != ESP_OK) goto flush_err;

        /* Data: control byte 0x40 + 128 pixels for this page */
        s_flush_buf[0] = 0x40u;
        memcpy(s_flush_buf + 1u, s_fb + (uint16_t)pg * FB_COLS, FB_COLS);
        err = i2c_master_transmit(s_dev, s_flush_buf, 1u + FB_COLS, I2C_TIMEOUT_MS);
        if (err != ESP_OK) goto flush_err;
    }
    s_flush_fail_cnt = 0u;  /* successful flush — reset error counter */
    return;

flush_err:
    /* Reset the I2C bus first — driver may be in INVALID_STATE after a timeout. */
    i2c_master_bus_reset(s_bus);
    s_flush_fail_cnt++;
    ESP_LOGW(TAG, "ssd1306_flush error (%s) — fail %u/3",
             esp_err_to_name(err), (unsigned)s_flush_fail_cnt);
    if (s_flush_fail_cnt >= 3u) {
        ESP_LOGE(TAG, "ssd1306_flush: 3 consecutive failures — disabling display");
        s_ok = false;
    }
}

/* ── Page renderers ──────────────────────────────────────────────────────── *
 *
 * Each renderer clears the framebuffer, writes text into s_fb[], then
 * returns.  ssd1306_flush() is called once by display_update() after.
 *
 * Layout (8 rows):
 *   Row 0: page title (full-width, inverted look via underline)
 *   Row 1: visual separator (thin horizontal line via fb_hline)
 *   Rows 2–7: content
 * ─────────────────────────────────────────────────────────────────────────── */

static void render_page_overview(const display_stats_t *s)
{
    char buf[22];

    /* Row 0: title */
    fb_str(0u, 0u, "OVERVIEW");

    /* Row 1: separator */
    fb_hline(1u);

    /* Row 2: NodeID (last 4 bytes as hex) */
    snprintf(buf, sizeof(buf), "ID: %08lX", (unsigned long)s->node_id);
    fb_str(0u, 2u, buf);

    /* Row 3: NetID */
    snprintf(buf, sizeof(buf), "NET:0x%04X", (unsigned)s->net_id);
    fb_str(0u, 3u, buf);

    /* Row 4: Callsign */
    snprintf(buf, sizeof(buf), "CS: %.11s", s->callsign[0] ? s->callsign : "---");
    fb_str(0u, 4u, buf);

    /* Row 5: Uptime */
    uint32_t h = s->uptime_s / 3600u;
    uint32_t m = (s->uptime_s % 3600u) / 60u;
    uint32_t sec = s->uptime_s % 60u;
    snprintf(buf, sizeof(buf), "UP: %lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    fb_str(0u, 5u, buf);
}

static void render_page_rf(const display_stats_t *s)
{
    char buf[22];

    fb_str(0u, 0u, "RF STATS");
    fb_hline(1u);

    snprintf(buf, sizeof(buf), "Now: %d dBm", (int)s->rssi_inst_dbm);
    fb_str(0u, 2u, buf);

    snprintf(buf, sizeof(buf), "Pkt: %d/%+ddB", (int)s->rssi_dbm, (int)s->snr_db);
    fb_str(0u, 3u, buf);

    snprintf(buf, sizeof(buf), "RX:  %lu", (unsigned long)s->rx_count);
    fb_str(0u, 4u, buf);

    snprintf(buf, sizeof(buf), "TX:  %lu", (unsigned long)s->tx_count);
    fb_str(0u, 5u, buf);
}

static void render_page_routing(const display_stats_t *s)
{
    char buf[22];

    fb_str(0u, 0u, "ROUTING");
    fb_hline(1u);

    snprintf(buf, sizeof(buf), "NBRS: %u", (unsigned)s->neighbor_count);
    fb_str(0u, 2u, buf);

    snprintf(buf, sizeof(buf), "RTES: %u", (unsigned)s->route_count);
    fb_str(0u, 3u, buf);

    snprintf(buf, sizeof(buf), "PEND: %u", (unsigned)s->pending_count);
    fb_str(0u, 4u, buf);
}

static void render_page_dutycycle(const display_stats_t *s)
{
    char buf[22];

    fb_str(0u, 0u, "DUTY CYCLE");
    fb_hline(1u);

    /* Used: e.g. 23 → "2.3%" */
    snprintf(buf, sizeof(buf), "USED: %u.%u%%",
             (unsigned)(s->dc_used_pct_x10 / 10u),
             (unsigned)(s->dc_used_pct_x10 % 10u));
    fb_str(0u, 2u, buf);

    uint16_t remaining = (s->dc_used_pct_x10 < 1000u)
                             ? (uint16_t)(1000u - s->dc_used_pct_x10)
                             : 0u;
    snprintf(buf, sizeof(buf), "REM:  %u.%u%%",
             (unsigned)(remaining / 10u),
             (unsigned)(remaining % 10u));
    fb_str(0u, 3u, buf);

    snprintf(buf, sizeof(buf), "BKOF: %lu ms", (unsigned long)s->dc_backoff_ms);
    fb_str(0u, 4u, buf);
}

static void render_page_vm(const display_stats_t *s)
{
    char buf[22];

    fb_str(0u, 0u, "RIVR VM");
    fb_hline(1u);

    snprintf(buf, sizeof(buf), "CYC: %lu", (unsigned long)s->vm_cycles);
    fb_str(0u, 2u, buf);

    snprintf(buf, sizeof(buf), "EVT: %.12s",
             s->last_event[0] ? s->last_event : "---");
    fb_str(0u, 3u, buf);

    snprintf(buf, sizeof(buf), "ERR: %lu", (unsigned long)s->error_code);
    fb_str(0u, 4u, buf);
    /* Page indicator handled by display_update() — VM is no longer the last page */
}

static void render_page_neighbors(const display_stats_t *s)
{
    char buf[22];

    fb_str(0u, 0u, "NEIGHBOURS");
    fb_hline(1u);

    /* Show up to DISPLAY_NEIGHBOR_MAX live neighbour entries */
    uint8_t shown = 0u;
    for (uint8_t i = 0u; i < DISPLAY_NEIGHBOR_MAX; i++) {
        if (!s->neighbors[i].valid) continue;

        uint8_t row = (uint8_t)(2u + shown * 2u);   /* rows 2,4,6 */
        if (row >= 8u) break;

        /* Row N: callsign + RSSI, e.g. "PA3ABC  -82dBm" */
        const char *cs = s->neighbors[i].callsign[0]
                         ? s->neighbors[i].callsign : "------";
        snprintf(buf, sizeof(buf), "%-6.6s %4ddBm", cs,
                 (int)s->neighbors[i].rssi_dbm);
        fb_str(0u, row, buf);

        /* Row N+1: hop count + age, e.g. "h=1  42s ago" */
        if (row + 1u < 8u) {
            uint32_t age = s->neighbors[i].age_s;
            if (age < 60u) {
                snprintf(buf, sizeof(buf), "h=%u  %lus ago",
                         (unsigned)s->neighbors[i].hop_count,
                         (unsigned long)age);
            } else {
                snprintf(buf, sizeof(buf), "h=%u  %lum ago",
                         (unsigned)s->neighbors[i].hop_count,
                         (unsigned long)(age / 60u));
            }
            fb_str(0u, (uint8_t)(row + 1u), buf);
        }
        shown++;
    }

    if (shown == 0u) {
        fb_str(2u, 3u, "no neighbours");
    }

    /* Last-page indicator: "6/6" in bottom-right corner */
    char pg[5];
    pg[0] = (char)('0' + (char)(s_page + 1u));
    pg[1] = '/';
    pg[2] = (char)('0' + (char)DISPLAY_PAGES);
    pg[3] = '\0';
    fb_str_right(7u, pg);
}

static void render_page_fabric(const display_stats_t *s)
{
    char buf[22];

    fb_str(0u, 0u, "FABRIC");
    fb_hline(1u);

    /* Row 2: score + decision band */
    const char *band =
        (s->fabric_score >= 80u) ? "DROP" :
        (s->fabric_score >= 50u) ? "DLY+" :
        (s->fabric_score >= 20u) ? "DLY " : "OK  ";
    snprintf(buf, sizeof(buf), "SCR:%3u %s", s->fabric_score, band);
    fb_str(0u, 2u, buf);

    /* Row 3: RX rate over last 60 s */
    snprintf(buf, sizeof(buf), "RX/s  %u.%02u",
             (unsigned)(s->fabric_rx_per_s_x100 / 100u),
             (unsigned)(s->fabric_rx_per_s_x100 % 100u));
    fb_str(0u, 3u, buf);

    /* Row 4: DC-blocked rate */
    snprintf(buf, sizeof(buf), "BLK/s %u.%02u",
             (unsigned)(s->fabric_blocked_per_s_x100 / 100u),
             (unsigned)(s->fabric_blocked_per_s_x100 % 100u));
    fb_str(0u, 4u, buf);

    /* Row 5: TX-fail rate */
    snprintf(buf, sizeof(buf), "FAIL/s %u.%02u",
             (unsigned)(s->fabric_fail_per_s_x100 / 100u),
             (unsigned)(s->fabric_fail_per_s_x100 % 100u));
    fb_str(0u, 5u, buf);

    /* Row 6: lifetime relay drop (D) and delay (Y) totals */
    snprintf(buf, sizeof(buf), "D:%lu Y:%lu",
             (unsigned long)s->fabric_relay_drop,
             (unsigned long)s->fabric_relay_delay);
    fb_str(0u, 6u, buf);

    /* Last-page indicator: "7/7" bottom-right */
    char pg[5];
    pg[0] = (char)('0' + (char)(s_page + 1u));
    pg[1] = '/';
    pg[2] = (char)('0' + (char)DISPLAY_PAGES);
    pg[3] = '\0';
    fb_str_right(7u, pg);
}

/** Render a boot screen (called once from display_init). */
static void render_boot(const display_stats_t *s)
{
    char buf[22];

    /* Row 0: big title — centred by padding */
    fb_str(3u, 0u, "RIVR NODE");
    fb_hline(1u);

    if (s) {
        snprintf(buf, sizeof(buf), "ID: %08lX", (unsigned long)s->node_id);
        fb_str(0u, 3u, buf);

        snprintf(buf, sizeof(buf), "NET:0x%04X", (unsigned)s->net_id);
        fb_str(0u, 4u, buf);

        snprintf(buf, sizeof(buf), "CS: %.11s",
                 s->callsign[0] ? s->callsign : "---");
        fb_str(0u, 5u, buf);
    } else {
        fb_str(3u, 3u, "initialising...");
    }
}

/* ── Shared stats buffer (written by main task, read by display task) ────── *
 *
 * No mutex: display_stats_t is small enough that a torn write produces only
 * a momentarily stale frame on screen, which is perfectly acceptable.
 * volatile ensures the compiler doesn't cache reads across the task boundary.
 * ─────────────────────────────────────────────────────────────────────────── */
static volatile display_stats_t s_shared_stats;
static volatile bool             s_stats_ready; /**< true after first post  */

/* ── Internal init (called from display task only) ───────────────────────── */

static void display_init(const display_stats_t *initial)
{
    s_ok = false;
    s_page = 0u;
    s_flush_fail_cnt = 0u;
    s_last_update_ms = 0u;
    s_last_rotate_ms = 0u;

    /* ── I2C bus ── */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = I2C_BUS_PORT,
        .sda_io_num          = PIN_DISPLAY_SDA,
        .scl_io_num          = PIN_DISPLAY_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7u,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return;
    }

    /* ── Power-on settling time ───────────────────────────────────────────────
     * SSD1306 needs ≥100 ms after VCC stable before accepting commands.    */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* ── Auto-detect I2C address: try 0x3C then 0x3D by actual transmit ─ *
     * This is more reliable than i2c_master_probe on noisy/slow lines.    */
    static const uint8_t k_addrs[] = {0x3Cu, 0x3Du};
    bool found = false;
    for (uint8_t ai = 0u; ai < 2u && !found; ai++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = k_addrs[ai],
            .scl_speed_hz    = I2C_BUS_SPEED_HZ,
        };
        err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "add_device(0x%02X) failed: %s", k_addrs[ai], esp_err_to_name(err));
            continue;
        }
        /* Ping: send display-off command; ACK proves device is present */
        uint8_t ping[2] = {0x00u, 0xAEu};
        err = i2c_master_transmit(s_dev, ping, sizeof(ping), I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SSD1306 found at 0x%02X", k_addrs[ai]);
            found = true;
        } else {
            ESP_LOGW(TAG, "0x%02X: no ACK (%s)", k_addrs[ai], esp_err_to_name(err));
            i2c_master_bus_rm_device(s_dev);
        }
    }
    if (!found) {
        ESP_LOGE(TAG,
            "SSD1306 not found on SDA=GPIO%d SCL=GPIO%d.\n"
            "  Check: VCC=3.3V, GND, SDA/SCL not swapped, pull-ups present.",
            PIN_DISPLAY_SDA, PIN_DISPLAY_SCL);
        i2c_del_master_bus(s_bus);
        return;
    }

    /* ── Full hardware init ── */
    err = ssd1306_hw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ssd1306_hw_init failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(s_bus);
        return;
    }
    s_ok = true;
    ESP_LOGI(TAG, "SSD1306 ready (SDA=%d SCL=%d speed=%u Hz)",
             PIN_DISPLAY_SDA, PIN_DISPLAY_SCL, I2C_BUS_SPEED_HZ);

    /* ── Self-test: force ALL pixels ON via register (no GDDRAM needed) ──────
     * 0xA5 = "Entire Display ON" — all pixels light up regardless of RAM.
     * If display shows white here → hardware path confirmed good.
     * If still black → I2C wiring / power problem, not firmware.          */
    ssd1306_cmd1(0xA5u);  /* entire display ON (ignores GDDRAM)            */
    ESP_LOGI(TAG, "Self-test: all pixels ON for %u ms — display MUST show white",
             (unsigned)SELFTEST_MS);
    vTaskDelay(pdMS_TO_TICKS(SELFTEST_MS));
    ssd1306_cmd1(0xA4u);  /* back to normal (follow GDDRAM)                */

    /* ── Boot screen ── */
    fb_clear();
    render_boot(initial);
    ssd1306_flush();

    s_last_rotate_ms = tb_millis();
    s_last_update_ms = tb_millis();
}

/* ── Internal update (called from display task only) ─────────────────────── */

static void display_update(const display_stats_t *stats)
{
    if (!s_ok || !stats) return;

    uint32_t now = tb_millis();

    /* ── 200 ms throttle ── */
    if ((now - s_last_update_ms) < DISPLAY_REFRESH_MS) return;
    s_last_update_ms = now;

    /* ── Auto-rotate page every 3 s ── */
    if ((now - s_last_rotate_ms) >= DISPLAY_PAGE_ROTATE_MS) {
        s_last_rotate_ms = now;
        s_page = (uint8_t)((s_page + 1u) % DISPLAY_PAGES);
    }

    /* ── Render active page ── */
    fb_clear();
    switch (s_page) {
        case 0u: render_page_overview(stats);   break;
        case 1u: render_page_rf(stats);         break;
        case 2u: render_page_routing(stats);    break;
        case 3u: render_page_dutycycle(stats);  break;
        case 4u: render_page_vm(stats);         break;
        case 5u: render_page_neighbors(stats);  break;
        case 6u: render_page_fabric(stats);     break;
        default: render_page_overview(stats);   break;
    }

    /* Page indicator: bottom-right corner on every page — 1-based, e.g. "1/5" */
    if (s_page != (DISPLAY_PAGES - 1u)) {   /* last page adds its own indicator */
        char pg[5];
        pg[0] = (char)('0' + (char)(s_page + 1u));
        pg[1] = '/';
        pg[2] = (char)('0' + (char)DISPLAY_PAGES);
        pg[3] = '\0';
        fb_str_right(7u, pg);
    }

    ssd1306_flush();
}

/* ── FreeRTOS display task ───────────────────────────────────────────────── *
 *
 * Runs at priority 1 (below main task priority).  Handles ALL I2C traffic.
 * If the bus hangs, only this task blocks; the main loop is unaffected.
 * ─────────────────────────────────────────────────────────────────────────── */
#define DISPLAY_TASK_STACK_WORDS  4096u  /**< 16 kB — ESP-IDF v5 I2C master driver uses deep stack */
#define DISPLAY_TASK_PRIORITY     1u

static void display_task(void *arg)
{
    (void)arg;

    /* Take a local copy of initial stats for the boot screen */
    display_stats_t local;
    memset(&local, 0, sizeof(local));
    if (s_stats_ready) {
        memcpy(&local, (const void *)&s_shared_stats, sizeof(local));
    }
    display_init(&local);

    for (;;) {
        /* Copy shared stats snapshot (cheaply; no mutex needed here) */
        if (s_stats_ready) {
            memcpy(&local, (const void *)&s_shared_stats, sizeof(local));
        }
        display_update(&local);

        /* Yield to other tasks; display_update throttles to 5 Hz internally */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void display_task_start(const display_stats_t *initial)
{
    /* Copy initial stats into shared buffer before spawning the task */
    if (initial) {
        memcpy((void *)&s_shared_stats, initial, sizeof(s_shared_stats));
        s_stats_ready = true;
    }

    static StaticTask_t s_task_buf;
    static StackType_t  s_task_stack[DISPLAY_TASK_STACK_WORDS];

    xTaskCreateStaticPinnedToCore(display_task,
                                   "display",
                                   DISPLAY_TASK_STACK_WORDS,
                                   NULL,
                                   DISPLAY_TASK_PRIORITY,
                                   s_task_stack,
                                   &s_task_buf,
                                   1);  /* CPU1 — keeps CPU0 free for main+IDLE0 */
}

void display_post_stats(const display_stats_t *stats)
{
    if (!stats) return;
    memcpy((void *)&s_shared_stats, stats, sizeof(s_shared_stats));
    s_stats_ready = true;
}

#endif /* FEATURE_DISPLAY */
