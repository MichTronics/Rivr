/**
 * @file  channel.h
 * @brief RIVR channel-based messaging model.
 *
 * DESIGN PRINCIPLES
 * ─────────────────
 * Channels are the primary messaging abstraction in Rivr.  Every user-visible
 * chat message belongs to exactly one channel.  Global/public chat is channel 0.
 * No heap allocation; all state lives in BSS.
 *
 * WIRE INTEGRATION
 * ─────────────────
 * The channel_id is carried in the PKT_CHAT payload when the sender sets
 * PKT_FLAG_CHANNEL (0x08) in the packet flags field:
 *
 *   flags & PKT_FLAG_CHANNEL set   → payload = [channel_id u16 LE][utf-8 text]
 *   flags & PKT_FLAG_CHANNEL clear → payload = [utf-8 text]  (legacy; channel 0)
 *
 * This keeps wire overhead minimal (2 bytes per channel message) and lets
 * v1 relay nodes forward channel messages transparently — they relay all
 * PKT_CHAT frames regardless of flags bits.
 *
 * RELAY vs. MEMBERSHIP SEPARATION
 * ─────────────────────────────────
 * Forwarding policy is strictly independent of channel membership:
 *   - A node ALWAYS relays PKT_CHAT frames it receives, regardless of whether
 *     it is joined to the embedded channel.  Relay pruning by channel would
 *     fragment the mesh delivery graph unacceptably.
 *   - Membership controls only local store + UI display:
 *       joined=true  → store message, emit CHAN_RX_UI_EVENT
 *       joined=false → relay only (still counted in per-channel rx metrics)
 *   - muted   → stored but not elevated in UI; unread count suppressed
 *   - hidden  → stored, not shown in default channel list
 *
 * PERSISTENCE FORMAT
 * ──────────────────
 * The channel table is saved as a flat binary blob via NVS.
 * Blob key: "rivr.channels"
 *
 *   [0]     magic     u8   = CHAN_PERSIST_MAGIC (0xC4)
 *   [1]     version   u8   = CHAN_PERSIST_VER   (1)
 *   [2]     count     u8   = number of entries saved (≤ RIVR_MAX_CHANNELS)
 *   [3]     crc8      u8   = CRC-8/MAXIM over bytes [0..2+count*sizeof(entry)]
 *   [4..]   entries        = array of rivr_channel_persist_entry_t
 *
 * If the blob is missing, corrupt (bad magic/version/crc), or count is out of
 * range, channel_persist_load() falls back silently to default config.
 *
 * DEFAULT CHANNEL TABLE
 * ─────────────────────
 *   ID 0 — "Global"    kind=Public,    joined=true,  muted=false, hidden=false
 *   ID 1 — "Ops"       kind=Group,     joined=true,  muted=false, hidden=false
 *   ID 2 — "Local"     kind=Group,     joined=false, muted=false, hidden=false
 *   ID 3 — "Emergency" kind=Emergency, joined=true,  muted=false, hidden=false
 *   ID 4..7 — empty slots (joined=false, hidden=true)
 *
 * SOFT PM / RESTRICTED CHANNELS
 * ───────────────────────────────
 * A "private conversation" is represented as a Restricted channel with:
 *   - kind = CHAN_KIND_RESTRICTED
 *   - optional key_slot > 0 (future encryption)
 *   - hidden = true (not shown in default list unless joined)
 *   - flag CHAN_FLAG_TWO_MEMBER set
 * This scaffolding avoids infecting the main chat path with PM-specific
 * special cases.  Full implementation is deferred; the wire format and
 * membership model support it cleanly.
 *
 * AIRTIME DISCIPLINE
 * ───────────────────
 * This module emits NO periodic channel-presence broadcasts.
 * All channel metadata (names, membership) is local-only.
 * Channel metadata synced to companions via the companion bus on connect.
 * No per-channel beacon storms; no additional traffic vs. legacy PKT_CHAT.
 */

#ifndef RIVR_CHANNEL_H
#define RIVR_CHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time bounds ─────────────────────────────────────────────────── */

/** Maximum number of channels in the local table. */
#define RIVR_MAX_CHANNELS        8u

/** Maximum channel name length (including NUL terminator). */
#define RIVR_CHANNEL_NAME_MAX   24u

/** Reserved channel ID: Global public chat (maps to legacy PKT_CHAT). */
#define RIVR_CHAN_GLOBAL         0u

/** Sentinel: no default TX channel selected. */
#define RIVR_CHAN_TX_NONE      0xFFu

/* ── Channel kind ────────────────────────────────────────────────────────── */

/**
 * Describes the operational class of a channel.
 * Stored as u8 in the persistence blob; do not reorder without bumping
 * CHAN_PERSIST_VER.
 */
typedef enum {
    CHAN_KIND_PUBLIC    = 0u,  /**< Open channel; anyone may receive            */
    CHAN_KIND_GROUP     = 1u,  /**< Team/group channel; subscription-gated UI   */
    CHAN_KIND_EMERGENCY = 2u,  /**< Priority channel; relay regardless of mute  */
    CHAN_KIND_SYSTEM    = 3u,  /**< Internal diagnostics / firmware messages     */
    CHAN_KIND_RESTRICTED = 4u, /**< Future: private/encrypted channel scaffold   */
} rivr_channel_kind_t;

/* ── Channel config flags ────────────────────────────────────────────────── */

/** Channel config flag: future encryption key slot is active. */
#define CHAN_FLAG_ENCRYPTED    0x0001u

/** Channel config flag: two-member semantics (soft PM scaffold). */
#define CHAN_FLAG_TWO_MEMBER   0x0002u

/** Channel config flag: emergency — relay even when relay budget exhausted.
 *  Relay layer must check this flag independently of CHAN_KIND_EMERGENCY. */
#define CHAN_FLAG_PRIORITY     0x0004u

/* ── Channel config ──────────────────────────────────────────────────────── */

/**
 * Static configuration for one channel.
 * Stored in BSS; references no heap.
 */
typedef struct {
    uint16_t id;                           /**< Channel ID (wire value)          */
    rivr_channel_kind_t kind;              /**< Channel operational class        */
    uint16_t flags;                        /**< CHAN_FLAG_* bitmask               */
    uint8_t  key_slot;                     /**< 0 = plaintext; >0 = key slot idx  */
    char     name[RIVR_CHANNEL_NAME_MAX];  /**< NUL-terminated display name      */
} rivr_channel_config_t;

/* ── Channel membership ──────────────────────────────────────────────────── */

/**
 * Local membership state for one channel.
 * Controls display + storage behavior; does NOT affect relay decisions.
 */
typedef struct {
    uint16_t channel_id;   /**< Matching channel ID                             */
    bool     joined;       /**< true → store + display received messages         */
    bool     muted;        /**< true → suppress unread count increment           */
    bool     hidden;       /**< true → omit from default channel list            */
    bool     tx_default;   /**< true → this is the current default TX channel    */
} rivr_channel_membership_t;

/* ── Per-channel runtime counters ────────────────────────────────────────── */

/**
 * Monotonically increasing counters per channel.
 * All in BSS; initialised to zero at startup.
 */
typedef struct {
    uint32_t rx_total;      /**< Channel messages received (relay+local)        */
    uint32_t rx_stored;     /**< Messages stored to local log (joined channels) */
    uint32_t rx_dropped;    /**< Messages dropped (not joined + not relay)       */
    uint32_t rx_dedup;      /**< Dedup suppressed (this channel, any frame)     */
    uint32_t tx_total;      /**< Messages originated on this channel            */
    uint32_t fwd_total;     /**< Relay forwards carrying this channel tag       */
    uint32_t unread;        /**< Unread messages (reset on channel open in UI)  */
} rivr_channel_counters_t;

/* ── Channel table ───────────────────────────────────────────────────────── */

/**
 * Complete local channel state.
 * One BSS-resident instance; initialised by channel_init().
 */
typedef struct {
    rivr_channel_config_t     config[RIVR_MAX_CHANNELS];
    rivr_channel_membership_t membership[RIVR_MAX_CHANNELS];
    rivr_channel_counters_t   counters[RIVR_MAX_CHANNELS];
    uint8_t                   count;        /**< Number of configured channels   */
    uint8_t                   tx_default;   /**< Index of default TX channel     */
} rivr_channel_table_t;

/* ── Persistence layout ──────────────────────────────────────────────────── */

#define CHAN_PERSIST_MAGIC  0xC4u
#define CHAN_PERSIST_VER    1u

/** One entry in the persistence blob per channel. */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t  kind;
    uint16_t flags;
    uint8_t  key_slot;
    char     name[RIVR_CHANNEL_NAME_MAX];
    uint8_t  joined;
    uint8_t  muted;
    uint8_t  hidden;
    uint8_t  tx_default;
} rivr_channel_persist_entry_t;

_Static_assert(sizeof(rivr_channel_persist_entry_t) ==
               (2u + 1u + 2u + 1u + RIVR_CHANNEL_NAME_MAX + 4u),
               "rivr_channel_persist_entry_t layout changed");

/* ── Global channel table instance ──────────────────────────────────────── */

/**
 * BSS-resident channel table.  All API functions operate on this instance.
 * Accessible read-only from outside via channel_get_table().
 */
extern rivr_channel_table_t g_channel_table;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise the channel table with compiled-in defaults.
 *
 * Must be called once before any other channel_* function.
 * Safe to call after channel_persist_load() — load restores saved state,
 * init restores defaults when no saved state exists.
 *
 * Default table:
 *   0 → Global    (Public,    joined=true)
 *   1 → Ops       (Group,     joined=true)
 *   2 → Local     (Group,     joined=false)
 *   3 → Emergency (Emergency, joined=true,  CHAN_FLAG_PRIORITY)
 *   4-7 → empty   (hidden=true, joined=false)
 */
void channel_init(void);

/**
 * @brief Return a pointer to the full channel table (read-only).
 */
const rivr_channel_table_t *channel_get_table(void);

/**
 * @brief Find config for a given channel_id.  Returns NULL if not found.
 */
const rivr_channel_config_t *channel_get_config(uint16_t channel_id);

/**
 * @brief Find membership state for a given channel_id.  Returns NULL if not found.
 */
const rivr_channel_membership_t *channel_get_membership(uint16_t channel_id);

/**
 * @brief Return per-channel counters for the given channel_id.
 *        Returns NULL if not found.
 */
rivr_channel_counters_t *channel_get_counters(uint16_t channel_id);

/**
 * @brief Returns true if the local node is joined to the channel.
 *        Unrecognised channel_ids return false.
 */
bool channel_is_joined(uint16_t channel_id);

/**
 * @brief Returns true if the channel is muted.
 *        Unrecognised channel_ids return false.
 */
bool channel_is_muted(uint16_t channel_id);

/**
 * @brief Returns true if the channel is hidden.
 *        Unrecognised channel_ids return true (safe default = don't show).
 */
bool channel_is_hidden(uint16_t channel_id);

/**
 * @brief Returns true if this channel should be relayed with priority.
 *        Checks CHAN_KIND_EMERGENCY or CHAN_FLAG_PRIORITY.
 */
bool channel_is_priority(uint16_t channel_id);

/**
 * @brief Join a channel (set joined=true).  No-op for unknown channel_ids.
 */
void channel_join(uint16_t channel_id);

/**
 * @brief Leave a channel (set joined=false).  No-op for unknown channel_ids.
 *        channel 0 (Global) cannot be left — silently ignored.
 */
void channel_leave(uint16_t channel_id);

/**
 * @brief Set mute state for a channel.  No-op for unknown channel_ids.
 */
void channel_set_muted(uint16_t channel_id, bool muted);

/**
 * @brief Set hidden state for a channel.  No-op for unknown channel_ids.
 */
void channel_set_hidden(uint16_t channel_id, bool hidden);

/**
 * @brief Set the default TX channel.  @p channel_id must be joined.
 *        Returns true on success, false if not joined or not found.
 */
bool channel_set_tx_default(uint16_t channel_id);

/**
 * @brief Return the current default TX channel ID.
 *        Returns RIVR_CHAN_GLOBAL if none explicitly set.
 */
uint16_t channel_get_tx_default(void);

/**
 * @brief Increment the unread counter for a channel.
 *        Suppressed if channel is muted.
 */
void channel_mark_unread(uint16_t channel_id);

/**
 * @brief Reset the unread counter for a channel to zero.
 */
void channel_clear_unread(uint16_t channel_id);

/**
 * @brief Print channel table summary via platform log (INFO level).
 */
void channel_print_table(void);

/**
 * @brief Save the channel table to NVS (platform-provided implementation).
 *        On Linux / test builds this is a no-op stub.
 * @return true on success.
 */
bool channel_persist_save(void);

/**
 * @brief Load the channel table from NVS.
 *        Falls back to defaults silently on any error.
 *        Must be called before channel_init() writes defaults if you want
 *        saved state to take effect.  If NVS is absent the table remains
 *        in its zero state; channel_init() then writes defaults.
 * @return true if saved config was found and loaded, false if defaults used.
 */
bool channel_persist_load(void);

/* ── Trace macros ────────────────────────────────────────────────────────── *
 *
 * Trace points instrument the full channel message lifecycle.  They compile
 * to zero code at RIVR_LOG_LEVEL ≥ RIVR_LEVEL_INFO; add overhead only in
 * debug/trace builds.  Use RIVR_LOGT so they are stripped in production.
 *
 *  CHAN_RX_FRAME          — raw frame arrived with PKT_FLAG_CHANNEL
 *  CHAN_RX_VALID          — frame passed CRC + version + type checks
 *  CHAN_RX_CHANNEL_RESOLVED — channel_id extracted and table entry found
 *  CHAN_RX_STORED         — message stored into local log
 *  CHAN_RX_UI_EVENT       — UI/companion event emitted
 *  CHAN_RX_DROPPED        — message dropped (not joined, membership filter)
 *  CHAN_TX                — outgoing channel message originated
 *  CHAN_FORWARD           — channel message relay forwarded
 *  CHAN_DROP              — relay drop (budget/TTL/dedup) for channel message
 *  CHAN_STORE             — message stored (alias of CHAN_RX_STORED)
 *  CHAN_EVENT_EMIT        — companion bus event emitted (alias of CHAN_RX_UI_EVENT)
 * ────────────────────────────────────────────────────────────────────────── */

#include "rivr_log.h"

#define CHAN_TAG "CHAN"

#define CHAN_RX_FRAME(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_RX_FRAME chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#define CHAN_RX_VALID(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_RX_VALID chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#define CHAN_RX_CHANNEL_RESOLVED(chan_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_RX_CHANNEL_RESOLVED chan=%u joined=%d", \
              (unsigned)(chan_id), (int)channel_is_joined(chan_id))

#define CHAN_RX_STORED(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_RX_STORED chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#define CHAN_RX_UI_EVENT(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_RX_UI_EVENT chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#define CHAN_RX_DROPPED(chan_id, src_id, reason) \
    RIVR_LOGT(CHAN_TAG, "CHAN_RX_DROPPED chan=%u src=0x%08x reason=%s", \
              (unsigned)(chan_id), (unsigned)(src_id), (reason))

#define CHAN_TX(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_TX chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#define CHAN_FORWARD(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_FORWARD chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#define CHAN_DROP(chan_id, src_id, reason) \
    RIVR_LOGT(CHAN_TAG, "CHAN_DROP chan=%u src=0x%08x reason=%s", \
              (unsigned)(chan_id), (unsigned)(src_id), (reason))

#define CHAN_STORE(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_STORE chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#define CHAN_EVENT_EMIT(chan_id, src_id) \
    RIVR_LOGT(CHAN_TAG, "CHAN_EVENT_EMIT chan=%u src=0x%08x", \
              (unsigned)(chan_id), (unsigned)(src_id))

#ifdef __cplusplus
}
#endif

#endif /* RIVR_CHANNEL_H */
