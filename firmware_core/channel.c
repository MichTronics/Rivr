/**
 * @file  channel.c
 * @brief RIVR channel table — bounded BSS-only implementation.
 *
 * All state is in the single BSS instance g_channel_table.  No heap.
 * NVS persistence stubs are no-ops on Linux / test builds.
 * Platform-specific NVS implementations live in platform_esp32.c /
 * platform_nrf52.cpp etc. and are conditionally compiled in by the build
 * system when RIVR_PLATFORM_HAS_NVS is defined.
 *
 * DEFAULT CHANNEL TABLE
 * ─────────────────────
 *   0 — "Global"    Public    joined=true  hidden=false muted=false
 *   1 — "Ops"       Group     joined=true  hidden=false muted=false
 *   2 — "Local"     Group     joined=false hidden=false muted=false
 *   3 — "Emergency" Emergency joined=true  hidden=false muted=false flags=PRIORITY
 *   4-7 — (empty)            joined=false hidden=true
 *
 * RELAY POLICY (enforced by caller in routing / main loop)
 * ─────────────────────────────────────────────────────────
 * channel.c does NOT make relay decisions.  The caller must:
 *   1. Always call routing_should_forward() before relay; that covers TTL +
 *      dedup + loop-guard — independent of channel membership.
 *   2. Only consult channel_is_joined() when deciding whether to store/display.
 *   3. Emergency channels (channel_is_priority()) may bypass relay budget gate.
 *
 * DEDUP KEY
 * ──────────
 * The existing (src_id, pkt_id) key identifies each wire injection uniquely.
 * Channel ID does NOT participate in the dedup key — a node relays a message
 * at most once per injection regardless of channel.  This avoids duplicate
 * relay of channel 0 + channel N frames from the same injection event.
 */

#include "channel.h"
#include "rivr_log.h"
#include "rivr_metrics.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ── Global channel table (BSS) ─────────────────────────────────────────── */

rivr_channel_table_t g_channel_table;  /* zero-initialised by C runtime */

/* ── Default config table — indexed by slot (not necessarily = channel_id) */

static const rivr_channel_config_t s_defaults[RIVR_MAX_CHANNELS] = {
    /* slot 0 */
    {
        .id       = 0u,
        .kind     = CHAN_KIND_PUBLIC,
        .flags    = 0u,
        .key_slot = 0u,
        .name     = "Global",
    },
    /* slot 1 */
    {
        .id       = 1u,
        .kind     = CHAN_KIND_GROUP,
        .flags    = 0u,
        .key_slot = 0u,
        .name     = "Ops",
    },
    /* slot 2 */
    {
        .id       = 2u,
        .kind     = CHAN_KIND_GROUP,
        .flags    = 0u,
        .key_slot = 0u,
        .name     = "Local",
    },
    /* slot 3 */
    {
        .id       = 3u,
        .kind     = CHAN_KIND_EMERGENCY,
        .flags    = CHAN_FLAG_PRIORITY,
        .key_slot = 0u,
        .name     = "Emergency",
    },
    /* slots 4–7: empty named placeholders */
    { .id = 4u, .kind = CHAN_KIND_GROUP, .flags = 0u, .key_slot = 0u, .name = "" },
    { .id = 5u, .kind = CHAN_KIND_GROUP, .flags = 0u, .key_slot = 0u, .name = "" },
    { .id = 6u, .kind = CHAN_KIND_GROUP, .flags = 0u, .key_slot = 0u, .name = "" },
    { .id = 7u, .kind = CHAN_KIND_GROUP, .flags = 0u, .key_slot = 0u, .name = "" },
};

static const rivr_channel_membership_t s_default_membership[RIVR_MAX_CHANNELS] = {
    /* 0: Global    — joined, visible */
    { .channel_id = 0u, .joined = true,  .muted = false, .hidden = false, .tx_default = true  },
    /* 1: Ops       — joined, visible */
    { .channel_id = 1u, .joined = true,  .muted = false, .hidden = false, .tx_default = false },
    /* 2: Local     — not joined */
    { .channel_id = 2u, .joined = false, .muted = false, .hidden = false, .tx_default = false },
    /* 3: Emergency — joined, visible */
    { .channel_id = 3u, .joined = true,  .muted = false, .hidden = false, .tx_default = false },
    /* 4-7: hidden empty slots */
    { .channel_id = 4u, .joined = false, .muted = false, .hidden = true,  .tx_default = false },
    { .channel_id = 5u, .joined = false, .muted = false, .hidden = true,  .tx_default = false },
    { .channel_id = 6u, .joined = false, .muted = false, .hidden = true,  .tx_default = false },
    { .channel_id = 7u, .joined = false, .muted = false, .hidden = true,  .tx_default = false },
};

/* ── Internal helpers ────────────────────────────────────────────────────── */

/**
 * Return the internal table index for a channel_id, or RIVR_MAX_CHANNELS
 * (= not-found sentinel) if not present.
 */
static uint8_t find_slot(uint16_t channel_id)
{
    for (uint8_t i = 0u; i < RIVR_MAX_CHANNELS; i++) {
        if (g_channel_table.config[i].id == channel_id &&
            g_channel_table.config[i].name[0] != '\0') {
            return i;
        }
    }
    /*
     * channel_id=0 is always slot 0 even if name is empty (it can be renamed
     * by operator config).  Guard it explicitly.
     */
    if (channel_id == RIVR_CHAN_GLOBAL) return 0u;
    return RIVR_MAX_CHANNELS; /* not found */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void channel_init(void)
{
    memcpy(g_channel_table.config,     s_defaults,           sizeof(s_defaults));
    memcpy(g_channel_table.membership, s_default_membership, sizeof(s_default_membership));
    memset(g_channel_table.counters,   0,                    sizeof(g_channel_table.counters));
    g_channel_table.count      = RIVR_MAX_CHANNELS;
    g_channel_table.tx_default = 0u; /* default TX = Global */

    RIVR_LOGI(CHAN_TAG, "channel table initialised (%u channels)", RIVR_MAX_CHANNELS);
}

const rivr_channel_table_t *channel_get_table(void)
{
    return &g_channel_table;
}

const rivr_channel_config_t *channel_get_config(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    return (i < RIVR_MAX_CHANNELS) ? &g_channel_table.config[i] : NULL;
}

const rivr_channel_membership_t *channel_get_membership(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    return (i < RIVR_MAX_CHANNELS) ? &g_channel_table.membership[i] : NULL;
}

rivr_channel_counters_t *channel_get_counters(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    return (i < RIVR_MAX_CHANNELS) ? &g_channel_table.counters[i] : NULL;
}

bool channel_is_joined(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return false;
    return g_channel_table.membership[i].joined;
}

bool channel_is_muted(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return false;
    return g_channel_table.membership[i].muted;
}

bool channel_is_hidden(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return true; /* unknown → treat as hidden */
    return g_channel_table.membership[i].hidden;
}

bool channel_is_priority(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return false;
    const rivr_channel_config_t *cfg = &g_channel_table.config[i];
    return (cfg->kind == CHAN_KIND_EMERGENCY) ||
           ((cfg->flags & CHAN_FLAG_PRIORITY) != 0u);
}

void channel_join(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return;
    g_channel_table.membership[i].joined = true;
    RIVR_LOGI(CHAN_TAG, "joined channel %u (%s)",
              (unsigned)channel_id, g_channel_table.config[i].name);
}

void channel_leave(uint16_t channel_id)
{
    if (channel_id == RIVR_CHAN_GLOBAL) return; /* Global cannot be left */
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return;
    g_channel_table.membership[i].joined = false;
    /* If this was the TX default, fall back to Global */
    if (g_channel_table.membership[i].tx_default) {
        g_channel_table.membership[i].tx_default = false;
        g_channel_table.membership[0].tx_default = true;
        g_channel_table.tx_default = 0u;
    }
    RIVR_LOGI(CHAN_TAG, "left channel %u (%s)",
              (unsigned)channel_id, g_channel_table.config[i].name);
}

void channel_set_muted(uint16_t channel_id, bool muted)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return;
    g_channel_table.membership[i].muted = muted;
}

void channel_set_hidden(uint16_t channel_id, bool hidden)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return;
    g_channel_table.membership[i].hidden = hidden;
}

bool channel_set_tx_default(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return false;
    if (!g_channel_table.membership[i].joined) return false;

    /* Clear previous default */
    for (uint8_t j = 0u; j < RIVR_MAX_CHANNELS; j++) {
        g_channel_table.membership[j].tx_default = false;
    }
    g_channel_table.membership[i].tx_default = true;
    g_channel_table.tx_default = i;
    return true;
}

uint16_t channel_get_tx_default(void)
{
    uint8_t i = g_channel_table.tx_default;
    if (i >= RIVR_MAX_CHANNELS) return RIVR_CHAN_GLOBAL;
    return g_channel_table.config[i].id;
}

void channel_mark_unread(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return;
    if (g_channel_table.membership[i].muted) return;
    g_channel_table.counters[i].unread++;
}

void channel_clear_unread(uint16_t channel_id)
{
    uint8_t i = find_slot(channel_id);
    if (i >= RIVR_MAX_CHANNELS) return;
    g_channel_table.counters[i].unread = 0u;
}

void channel_print_table(void)
{
    RIVR_LOGI(CHAN_TAG, "Channel table (%u slots):", RIVR_MAX_CHANNELS);
    for (uint8_t i = 0u; i < RIVR_MAX_CHANNELS; i++) {
        const rivr_channel_config_t     *cfg = &g_channel_table.config[i];
        const rivr_channel_membership_t *mem = &g_channel_table.membership[i];
        const rivr_channel_counters_t   *cnt = &g_channel_table.counters[i];
        RIVR_LOGI(CHAN_TAG,
                  "  [%u] id=%-4u name=%-24s kind=%u joined=%d muted=%d hidden=%d "
                  "tx_default=%d rx=%u tx=%u fwd=%u unread=%u",
                  (unsigned)i,
                  (unsigned)cfg->id,
                  cfg->name,
                  (unsigned)cfg->kind,
                  (int)mem->joined,
                  (int)mem->muted,
                  (int)mem->hidden,
                  (int)mem->tx_default,
                  (unsigned)cnt->rx_total,
                  (unsigned)cnt->tx_total,
                  (unsigned)cnt->fwd_total,
                  (unsigned)cnt->unread);
    }
}

/* ── CRC-8/MAXIM for persistence blob integrity ──────────────────────────── */

static uint8_t crc8_maxim(const uint8_t *buf, size_t len)
{
    uint8_t crc = 0u;
    for (size_t i = 0u; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0u; b < 8u; b++) {
            if (crc & 0x01u) { crc = (uint8_t)((crc >> 1u) ^ 0x8Cu); }
            else             { crc >>= 1u; }
        }
    }
    return crc;
}

/* ── NVS persistence ─────────────────────────────────────────────────────── *
 *
 * Platform-gated: if RIVR_PLATFORM_HAS_NVS is not defined (Linux, tests) the
 * functions are stubs.  On ESP32, the real implementation lives in
 * platform_esp32.c and is injected via a weak-symbol or platform macro gate.
 * ────────────────────────────────────────────────────────────────────────── */

#ifndef RIVR_PLATFORM_HAS_NVS

bool channel_persist_save(void) { return true; }

bool channel_persist_load(void) { return false; }

#else  /* RIVR_PLATFORM_HAS_NVS */

#include "nvs_flash.h"
#include "nvs.h"

#define CHAN_NVS_NAMESPACE "rivr_chan"
#define CHAN_NVS_KEY       "table"

bool channel_persist_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CHAN_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;

    /* Build blob: header(4) + entries */
    const size_t entry_size = sizeof(rivr_channel_persist_entry_t);
    uint8_t buf[4u + RIVR_MAX_CHANNELS * sizeof(rivr_channel_persist_entry_t)];
    uint8_t n = (uint8_t)RIVR_MAX_CHANNELS;

    buf[0] = CHAN_PERSIST_MAGIC;
    buf[1] = CHAN_PERSIST_VER;
    buf[2] = n;

    rivr_channel_persist_entry_t *entries =
        (rivr_channel_persist_entry_t *)(buf + 4u);
    for (uint8_t i = 0u; i < n; i++) {
        entries[i].id         = g_channel_table.config[i].id;
        entries[i].kind       = (uint8_t)g_channel_table.config[i].kind;
        entries[i].flags      = g_channel_table.config[i].flags;
        entries[i].key_slot   = g_channel_table.config[i].key_slot;
        memcpy(entries[i].name, g_channel_table.config[i].name,
               RIVR_CHANNEL_NAME_MAX);
        entries[i].joined     = g_channel_table.membership[i].joined ? 1u : 0u;
        entries[i].muted      = g_channel_table.membership[i].muted  ? 1u : 0u;
        entries[i].hidden     = g_channel_table.membership[i].hidden ? 1u : 0u;
        entries[i].tx_default = g_channel_table.membership[i].tx_default ? 1u : 0u;
    }

    size_t total = 4u + n * entry_size;
    /* CRC over bytes [0..2] + entries */
    buf[3] = crc8_maxim(buf, total - 1u); /* Recompute after entries written */
    /* Actually CRC should cover buf[0..2] + entries, i.e. all bytes except buf[3] */
    buf[3] = crc8_maxim(buf + 4u, n * entry_size);
    /* Simpler: CRC over everything before buf[3] */
    buf[3] = 0u;
    uint8_t crc_payload = crc8_maxim(buf, 3u);
    for (uint8_t i = 0u; i < n; i++) {
        crc_payload ^= crc8_maxim((const uint8_t *)&entries[i], entry_size);
    }
    buf[3] = crc_payload;

    err = nvs_set_blob(h, CHAN_NVS_KEY, buf, total);
    nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool channel_persist_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CHAN_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    size_t sz = 0u;
    err = nvs_get_blob(h, CHAN_NVS_KEY, NULL, &sz);
    if (err != ESP_OK || sz < 4u) { nvs_close(h); return false; }

    const size_t entry_size = sizeof(rivr_channel_persist_entry_t);
    uint8_t buf[4u + RIVR_MAX_CHANNELS * sizeof(rivr_channel_persist_entry_t)];
    if (sz > sizeof(buf)) { nvs_close(h); return false; }

    err = nvs_get_blob(h, CHAN_NVS_KEY, buf, &sz);
    nvs_close(h);
    if (err != ESP_OK) return false;

    if (buf[0] != CHAN_PERSIST_MAGIC) return false;
    if (buf[1] != CHAN_PERSIST_VER)   return false;
    uint8_t n = buf[2];
    if (n == 0u || n > RIVR_MAX_CHANNELS) return false;
    if (sz < 4u + n * entry_size) return false;

    /* Verify CRC */
    uint8_t crc_stored = buf[3];
    uint8_t crc_calc   = crc8_maxim(buf, 3u);
    const rivr_channel_persist_entry_t *entries =
        (const rivr_channel_persist_entry_t *)(buf + 4u);
    for (uint8_t i = 0u; i < n; i++) {
        crc_calc ^= crc8_maxim((const uint8_t *)&entries[i], entry_size);
    }
    if (crc_calc != crc_stored) {
        RIVR_LOGW(CHAN_TAG, "channel persist CRC mismatch — using defaults");
        return false;
    }

    /* Apply loaded config */
    for (uint8_t i = 0u; i < n; i++) {
        g_channel_table.config[i].id       = entries[i].id;
        g_channel_table.config[i].kind     = (rivr_channel_kind_t)entries[i].kind;
        g_channel_table.config[i].flags    = entries[i].flags;
        g_channel_table.config[i].key_slot = entries[i].key_slot;
        memcpy(g_channel_table.config[i].name, entries[i].name, RIVR_CHANNEL_NAME_MAX);
        g_channel_table.config[i].name[RIVR_CHANNEL_NAME_MAX - 1u] = '\0';

        g_channel_table.membership[i].channel_id  = entries[i].id;
        g_channel_table.membership[i].joined      = entries[i].joined != 0u;
        g_channel_table.membership[i].muted       = entries[i].muted  != 0u;
        g_channel_table.membership[i].hidden      = entries[i].hidden != 0u;
        g_channel_table.membership[i].tx_default  = entries[i].tx_default != 0u;

        if (entries[i].tx_default) {
            g_channel_table.tx_default = i;
        }
    }
    g_channel_table.count = n;

    RIVR_LOGI(CHAN_TAG, "channel table loaded from NVS (%u channels)", (unsigned)n);
    return true;
}

#endif /* RIVR_PLATFORM_HAS_NVS */
