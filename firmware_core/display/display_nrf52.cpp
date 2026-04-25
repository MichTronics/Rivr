/**
 * @file  display/display_nrf52.cpp
 * @brief nRF52 SSD1306 OLED driver using Arduino Wire.
 *
 * Mirrors the public display API from display.h for nRF52840 builds. The
 * rendering model stays intentionally close to the ESP32 implementation:
 * static framebuffer, 5x8 ASCII font, 200 ms refresh guard, and a dedicated
 * low-priority display task.
 */

#if defined(RIVR_PLATFORM_NRF52840)

#include "display.h"

#if FEATURE_DISPLAY

#include <Arduino.h>
#include <Wire.h>

extern "C" {
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../timebase.h"
}

#define FB_COLS            128u
#define FB_PAGES           8u
#define FB_BYTES           (FB_COLS * FB_PAGES)
#define FONT_FIRST         0x20u
#define FONT_LAST          0x7Eu
#define CHAR_W             5u
#define CHAR_STRIDE        6u
#define CHARS_PER_ROW      (FB_COLS / CHAR_STRIDE)
#define SELFTEST_MS        250u
#define OLED_DATA_CHUNK    16u

static const uint8_t FONT5X8[][CHAR_W] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x04,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00},
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02}, {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x40,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x20,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00},
    {0x08,0x08,0x2A,0x1C,0x08},
};

static uint8_t s_fb[FB_BYTES];
static uint8_t s_page;
static bool s_ok;
static uint8_t s_i2c_addr = DISPLAY_I2C_ADDR;
static uint32_t s_last_update_ms;
static uint32_t s_last_rotate_ms;
static volatile display_stats_t s_shared_stats;
static volatile bool s_stats_ready;
static bool s_task_started;

static bool ssd1306_send(const uint8_t *buf, size_t len)
{
    Wire.beginTransmission(s_i2c_addr);
    size_t written = Wire.write(buf, len);
    return (written == len) && (Wire.endTransmission() == 0);
}

static bool ssd1306_cmd1(uint8_t c)
{
    const uint8_t b[2] = {0x00u, c};
    return ssd1306_send(b, sizeof(b));
}

static bool ssd1306_cmd2(uint8_t c, uint8_t d)
{
    const uint8_t b[3] = {0x00u, c, d};
    return ssd1306_send(b, sizeof(b));
}

static bool ssd1306_cmd3(uint8_t c, uint8_t d0, uint8_t d1)
{
    const uint8_t b[4] = {0x00u, c, d0, d1};
    return ssd1306_send(b, sizeof(b));
}

static bool ssd1306_probe(uint8_t addr)
{
    s_i2c_addr = addr;
    return ssd1306_cmd1(0xAEu);
}

static bool ssd1306_hw_init(void)
{
    return ssd1306_cmd1(0xAEu) &&
           ssd1306_cmd2(0xD5u, 0x80u) &&
           ssd1306_cmd2(0xA8u, 0x3Fu) &&
           ssd1306_cmd2(0xD3u, 0x00u) &&
           ssd1306_cmd1(0x40u) &&
           ssd1306_cmd2(0x8Du, 0x14u) &&
           ssd1306_cmd2(0x20u, 0x00u) &&
           ssd1306_cmd1(0xA1u) &&
           ssd1306_cmd1(0xC8u) &&
           ssd1306_cmd2(0xDAu, 0x12u) &&
           ssd1306_cmd2(0x81u, 0xFFu) &&
           ssd1306_cmd2(0xD9u, 0xF1u) &&
           ssd1306_cmd2(0xDBu, 0x20u) &&
           ssd1306_cmd1(0xA4u) &&
           ssd1306_cmd1(0xA6u) &&
           ssd1306_cmd1(0xAFu);
}

static void fb_clear(void)
{
    memset(s_fb, 0x00u, sizeof(s_fb));
}

static void fb_char(uint8_t col, uint8_t row, char c)
{
    if ((uint8_t)c < FONT_FIRST || (uint8_t)c > FONT_LAST) c = ' ';
    const uint8_t *g = FONT5X8[(uint8_t)c - FONT_FIRST];
    const uint16_t px = (uint16_t)col * CHAR_STRIDE;
    if (px + CHAR_W > FB_COLS) return;
    const uint16_t base = (uint16_t)row * FB_COLS;
    for (uint8_t i = 0u; i < CHAR_W; i++) {
        s_fb[base + px + i] = g[i];
    }
    s_fb[base + px + CHAR_W] = 0x00u;
}

static void fb_str(uint8_t col, uint8_t row, const char *s)
{
    while (s && *s && col < CHARS_PER_ROW) {
        fb_char(col++, row, *s++);
    }
}

static void fb_str_right(uint8_t row, const char *s)
{
    if (!s) return;
    const size_t len = strlen(s);
    const uint8_t clipped = (len > CHARS_PER_ROW) ? CHARS_PER_ROW : (uint8_t)len;
    const uint8_t col = (CHARS_PER_ROW >= clipped) ? (uint8_t)(CHARS_PER_ROW - clipped) : 0u;
    fb_str(col, row, s);
}

static void fb_hline(uint8_t row)
{
    memset(s_fb + (uint16_t)row * FB_COLS, 0x01u, FB_COLS);
}

static void render_page_overview(const display_stats_t *s)
{
    char buf[22];
    fb_str(0u, 0u, "OVERVIEW");
    fb_hline(1u);
    snprintf(buf, sizeof(buf), "ID: %08lX", (unsigned long)s->node_id);
    fb_str(0u, 2u, buf);
    snprintf(buf, sizeof(buf), "NET:0x%04X", (unsigned)s->net_id);
    fb_str(0u, 3u, buf);
    snprintf(buf, sizeof(buf), "CS: %.11s", s->callsign[0] ? s->callsign : "---");
    fb_str(0u, 4u, buf);
    const uint32_t h = s->uptime_s / 3600u;
    const uint32_t m = (s->uptime_s % 3600u) / 60u;
    const uint32_t sec = s->uptime_s % 60u;
    snprintf(buf, sizeof(buf), "UP: %lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    fb_str(0u, 5u, buf);
    if (s->ble_connected) {
        fb_str(0u, 6u, "BLE: CONNECTED");
    } else if (s->ble_passkey != 0u && s->ble_active && !s->ble_paired) {
        snprintf(buf, sizeof(buf), "PIN:%06lu", (unsigned long)s->ble_passkey);
        fb_str(0u, 6u, buf);
    } else if (s->ble_paired && s->ble_active) {
        fb_str(0u, 6u, "BLE: PAIRED");
    } else if (s->ble_paired) {
        fb_str(0u, 6u, "BLE: PAIRED OFF");
    } else if (s->ble_active) {
        fb_str(0u, 6u, "BLE: OPEN");
    } else {
        fb_str(0u, 6u, "BLE: OFF");
    }
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
    snprintf(buf, sizeof(buf), "USED: %u.%u%%",
             (unsigned)(s->dc_used_pct_x10 / 10u),
             (unsigned)(s->dc_used_pct_x10 % 10u));
    fb_str(0u, 2u, buf);
    const uint16_t remaining = (s->dc_used_pct_x10 < 1000u)
                                   ? (uint16_t)(1000u - s->dc_used_pct_x10) : 0u;
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
    snprintf(buf, sizeof(buf), "EVT: %.12s", s->last_event[0] ? s->last_event : "---");
    fb_str(0u, 3u, buf);
    snprintf(buf, sizeof(buf), "ERR: %lu", (unsigned long)s->error_code);
    fb_str(0u, 4u, buf);
}

static void render_page_neighbors(const display_stats_t *s)
{
    char buf[22];
    fb_str(0u, 0u, "NEIGHBOURS");
    fb_hline(1u);
    uint8_t shown = 0u;
    for (uint8_t i = 0u; i < DISPLAY_NEIGHBOR_MAX; i++) {
        if (!s->neighbors[i].valid) continue;
        const uint8_t row = (uint8_t)(2u + shown * 2u);
        if (row >= 8u) break;
        const char *cs = s->neighbors[i].callsign[0] ? s->neighbors[i].callsign : "------";
        snprintf(buf, sizeof(buf), "%-6.6s %4ddBm", cs, (int)s->neighbors[i].rssi_dbm);
        fb_str(0u, row, buf);
        if (row + 1u < 8u) {
            const uint32_t age = s->neighbors[i].age_s;
            if (age < 60u) {
                snprintf(buf, sizeof(buf), "h=%u  %lus ago",
                         (unsigned)s->neighbors[i].hop_count, (unsigned long)age);
            } else {
                snprintf(buf, sizeof(buf), "h=%u  %lum ago",
                         (unsigned)s->neighbors[i].hop_count, (unsigned long)(age / 60u));
            }
            fb_str(0u, (uint8_t)(row + 1u), buf);
        }
        shown++;
    }
    if (shown == 0u) {
        fb_str(2u, 3u, "no neighbours");
    }
}

#if DISPLAY_HAS_SENSORS
static void render_page_sensors(const display_stats_t *s)
{
    char buf[32];

    fb_str(0u, 0u, "SENSORS");
    fb_hline(1u);

    uint8_t row = 2u;

#if RIVR_FEATURE_DS18B20
    if (s->sensor_ds18b20_valid) {
        int32_t v = s->sensor_ds18b20_temp_x100;
        bool ng = (v < 0); int32_t av = ng ? -v : v;
        snprintf(buf, sizeof(buf), "DS18B20 %s%ld.%02ld C",
                 ng ? "-" : "", (long)(av / 100), (long)(av % 100));
    } else {
        snprintf(buf, sizeof(buf), "DS18B20 ---");
    }
    if (row < 8u) { fb_str(0u, row++, buf); }
#endif

#if RIVR_FEATURE_AM2302
    if (s->sensor_am2302_rh_valid) {
        snprintf(buf, sizeof(buf), "RH   %lu.%02lu %%",
                 (unsigned long)(s->sensor_am2302_rh_x100 / 100u),
                 (unsigned long)(s->sensor_am2302_rh_x100 % 100u));
    } else {
        snprintf(buf, sizeof(buf), "RH   ---");
    }
    if (row < 8u) { fb_str(0u, row++, buf); }

    if (s->sensor_am2302_temp_valid) {
        int32_t t = s->sensor_am2302_temp_x100;
        bool ng = (t < 0); int32_t at = ng ? -t : t;
        snprintf(buf, sizeof(buf), "TEMP %s%ld.%02ld C",
                 ng ? "-" : "", (long)(at / 100), (long)(at % 100));
    } else {
        snprintf(buf, sizeof(buf), "TEMP ---");
    }
    if (row < 8u) { fb_str(0u, row++, buf); }
#endif

#if RIVR_FEATURE_VBAT
    if (s->sensor_vbat_valid) {
        snprintf(buf, sizeof(buf), "VBAT %ldmV", (long)s->sensor_vbat_mv);
    } else {
        snprintf(buf, sizeof(buf), "VBAT ---");
    }
    if (row < 8u) { fb_str(0u, row++, buf); }
#endif

    (void)row;
    (void)s;
}
#endif /* DISPLAY_HAS_SENSORS */

#if DISPLAY_HAS_FABRIC
static void render_page_fabric(const display_stats_t *s)
{
    char buf[22];
    fb_str(0u, 0u, "FABRIC");
    fb_hline(1u);
    const char *band =
        (s->fabric_score >= 80u) ? "DROP" :
        (s->fabric_score >= 50u) ? "DLY+" :
        (s->fabric_score >= 20u) ? "DLY " : "OK  ";
    snprintf(buf, sizeof(buf), "SCR:%3u %s", s->fabric_score, band);
    fb_str(0u, 2u, buf);
    snprintf(buf, sizeof(buf), "RX/s  %u.%02u",
             (unsigned)(s->fabric_rx_per_s_x100 / 100u),
             (unsigned)(s->fabric_rx_per_s_x100 % 100u));
    fb_str(0u, 3u, buf);
    snprintf(buf, sizeof(buf), "BLK/s %u.%02u",
             (unsigned)(s->fabric_blocked_per_s_x100 / 100u),
             (unsigned)(s->fabric_blocked_per_s_x100 % 100u));
    fb_str(0u, 4u, buf);
    snprintf(buf, sizeof(buf), "FAIL/s %u.%02u",
             (unsigned)(s->fabric_fail_per_s_x100 / 100u),
             (unsigned)(s->fabric_fail_per_s_x100 % 100u));
    fb_str(0u, 5u, buf);
    snprintf(buf, sizeof(buf), "D:%lu Y:%lu",
             (unsigned long)s->fabric_relay_drop,
             (unsigned long)s->fabric_relay_delay);
    fb_str(0u, 6u, buf);
}
#endif /* DISPLAY_HAS_FABRIC */

static void render_boot(const display_stats_t *s)
{
    char buf[22];
    fb_str(3u, 0u, "RIVR NODE");
    fb_hline(1u);
    if (s) {
        snprintf(buf, sizeof(buf), "ID: %08lX", (unsigned long)s->node_id);
        fb_str(0u, 3u, buf);
        snprintf(buf, sizeof(buf), "NET:0x%04X", (unsigned)s->net_id);
        fb_str(0u, 4u, buf);
        snprintf(buf, sizeof(buf), "CS: %.11s", s->callsign[0] ? s->callsign : "---");
        fb_str(0u, 5u, buf);
    } else {
        fb_str(2u, 3u, "initialising...");
    }
}

static void ssd1306_flush(void)
{
    if (!ssd1306_cmd3(0x22u, 0x00u, (uint8_t)(FB_PAGES - 1u))) {
        s_ok = false;
        return;
    }
    if (!ssd1306_cmd3(0x21u, 0x00u, (uint8_t)(FB_COLS - 1u))) {
        s_ok = false;
        return;
    }
    for (uint16_t off = 0u; off < FB_BYTES; off += OLED_DATA_CHUNK) {
        const uint8_t chunk = (uint8_t)(((FB_BYTES - off) > OLED_DATA_CHUNK) ? OLED_DATA_CHUNK : (FB_BYTES - off));
        Wire.beginTransmission(s_i2c_addr);
        Wire.write((uint8_t)0x40u);
        Wire.write(s_fb + off, chunk);
        if (Wire.endTransmission() != 0) {
            s_ok = false;
            return;
        }
    }
}

static void display_init_internal(const display_stats_t *initial)
{
    s_ok = false;
    s_page = 0u;
    s_last_update_ms = 0u;
    s_last_rotate_ms = 0u;

    Wire.begin();
    Wire.setClock(400000u);
    vTaskDelay(pdMS_TO_TICKS(120));

    if (!ssd1306_probe(DISPLAY_I2C_ADDR) && !ssd1306_probe(0x3Du)) {
        return;
    }
    if (!ssd1306_hw_init()) {
        return;
    }

    s_ok = true;
    ssd1306_cmd1(0xA5u);
    vTaskDelay(pdMS_TO_TICKS(SELFTEST_MS));
    ssd1306_cmd1(0xA4u);

    fb_clear();
    render_boot(initial);
    ssd1306_flush();
    s_last_rotate_ms = tb_millis();
    s_last_update_ms = tb_millis();
}

static void display_update_internal(const display_stats_t *stats)
{
    if (!s_ok || !stats) return;
    const uint32_t now = tb_millis();
    if ((now - s_last_update_ms) < DISPLAY_REFRESH_MS) return;
    s_last_update_ms = now;

    const bool show_ble_pin = stats->ble_active && !stats->ble_connected &&
                              !stats->ble_paired && (stats->ble_passkey != 0u);
    if (show_ble_pin) {
        s_page = 0u;
        s_last_rotate_ms = now;
    } else if ((now - s_last_rotate_ms) >= DISPLAY_PAGE_ROTATE_MS) {
        s_last_rotate_ms = now;
        s_page = (uint8_t)((s_page + 1u) % DISPLAY_PAGES);
    }

    fb_clear();
    switch (s_page) {
        case 0u: render_page_overview(stats); break;
        case 1u: render_page_rf(stats); break;
        case 2u: render_page_routing(stats); break;
        case 3u: render_page_dutycycle(stats); break;
        case 4u: render_page_vm(stats); break;
        case 5u: render_page_neighbors(stats); break;
#if DISPLAY_HAS_SENSORS
        case DISP_PAGE_SENSORS: render_page_sensors(stats); break;
#endif
#if DISPLAY_HAS_FABRIC
        case DISP_PAGE_FABRIC:  render_page_fabric(stats);  break;
#endif
        default: render_page_overview(stats); break;
    }

    char pg[5];
    pg[0] = (char)('0' + (char)(s_page + 1u));
    pg[1] = '/';
    pg[2] = (char)('0' + (char)DISPLAY_PAGES);
    pg[3] = '\0';
    fb_str_right(7u, pg);
    ssd1306_flush();
}

static void display_task(void *arg)
{
    (void)arg;
    display_stats_t local;
    memset(&local, 0, sizeof(local));
    if (s_stats_ready) {
        memcpy(&local, (const void *)&s_shared_stats, sizeof(local));
    }
    display_init_internal(&local);
    for (;;) {
        if (s_stats_ready) {
            memcpy(&local, (const void *)&s_shared_stats, sizeof(local));
        }
        display_update_internal(&local);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void display_task_start(const display_stats_t *initial)
{
    if (initial) {
        memcpy((void *)&s_shared_stats, initial, sizeof(s_shared_stats));
        s_stats_ready = true;
    }
    if (s_task_started) return;
    s_task_started = true;
    xTaskCreate(display_task, "display", 4096u / sizeof(StackType_t), NULL, 1, NULL);
}

extern "C" void display_post_stats(const display_stats_t *stats)
{
    if (!stats) return;
    memcpy((void *)&s_shared_stats, stats, sizeof(s_shared_stats));
    s_stats_ready = true;
}

#endif /* FEATURE_DISPLAY */
#endif /* RIVR_PLATFORM_NRF52840 */
