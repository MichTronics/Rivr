#include "rml.h"

#if RIVR_ENABLE_RML

#include "firmware_core/rivr_log.h"

#include <string.h>

#define TAG "RML"

typedef struct {
    uint8_t intent;
    uint8_t max_relay_ttl;
    uint8_t min_priority;
    uint16_t base_delay_ms;
    uint16_t jitter_ms;
    uint8_t airtime_floor_pct;
    bool relay_by_default;
    bool drop_under_pressure;
} rml_intent_policy_t;

typedef struct {
    uint16_t sender_id;
    uint16_t state_type;
    uint32_t newest_msg_id;
    bool used;
} rml_status_slot_t;

static const rml_intent_policy_t s_intent_policy[] = {
    { RML_INTENT_CHAT,      8u, 3u,  90u, 330u, 15u, true,  false },
    { RML_INTENT_PRIVATE,   8u, 4u,  60u, 220u, 12u, false, false },
    { RML_INTENT_GROUP,     4u, 4u, 120u, 260u, 20u, true,  false },
    { RML_INTENT_EMERGENCY, 8u, 9u,   0u,  35u,  1u, true,  false },
    { RML_INTENT_TELEMETRY, 3u, 1u, 450u, 700u, 35u, true,  true  },
    { RML_INTENT_STATUS,    2u, 2u, 500u, 600u, 30u, true,  true  },
    { RML_INTENT_SYSTEM,    3u, 5u,  80u, 160u, 10u, true,  false },
};

static rml_status_slot_t s_status[RML_STATUS_CACHE_SIZE];
static rml_thread_cache_t s_thread_cache;
static rml_repair_request_t s_repair_cache[RML_REPAIR_CACHE_SIZE];
static uint16_t s_repair_next;

void rml_policy_reset(void)
{
    memset(s_status, 0, sizeof(s_status));
    rml_thread_cache_reset();
    memset(s_repair_cache, 0, sizeof(s_repair_cache));
    s_repair_next = 0u;
}

void rml_thread_cache_reset(void)
{
    memset(&s_thread_cache, 0, sizeof(s_thread_cache));
}

static rml_policy_decision_t rml_decision(bool accept,
                                          bool relay,
                                          uint16_t delay_ms,
                                          uint8_t next_ttl,
                                          uint8_t reason)
{
    rml_policy_decision_t d;
    d.accept = accept;
    d.relay = relay;
    d.delay_ms = delay_ms;
    d.next_ttl = next_ttl;
    d.reason = reason;
    d.repair_delay_ms = 0u;
    d.forward_reconstructed = false;
    d.repair_needed = false;
    memset(&d.repair, 0, sizeof(d.repair));
    return d;
}

static rml_policy_decision_t rml_repair_decision(uint32_t missing_msg_id,
                                                 uint32_t thread_id,
                                                 uint16_t sender_id,
                                                 uint8_t reason)
{
    rml_policy_decision_t d = rml_decision(false, false, 0u, 0u, reason);
    d.repair_needed = true;
    d.repair_delay_ms = 750u;
    d.repair.missing_msg_id = missing_msg_id;
    d.repair.thread_id = thread_id;
    d.repair.sender_id = sender_id;

    s_repair_cache[s_repair_next] = d.repair;
    s_repair_next = (uint16_t)((s_repair_next + 1u) % (uint16_t)RML_REPAIR_CACHE_SIZE);
    return d;
}

static const rml_intent_policy_t *rml_policy_for_intent(uint8_t intent)
{
    for (uint8_t i = 0u; i < (uint8_t)(sizeof(s_intent_policy) / sizeof(s_intent_policy[0])); i++) {
        if (s_intent_policy[i].intent == intent) {
            return &s_intent_policy[i];
        }
    }
    return &s_intent_policy[0];
}

static uint32_t rml_mix32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

static uint16_t rml_delay_for(const rml_message_t *msg,
                              const rml_context_t *ctx,
                              const rml_intent_policy_t *policy,
                              uint8_t effective_priority)
{
    uint32_t seed = msg->msg_id
                  ^ (msg->thread_id * 2654435761u)
                  ^ ((uint32_t)msg->sender_id << 16)
                  ^ (uint32_t)msg->target_id;
    if (ctx) {
        seed ^= ctx->now_ms;
        seed ^= ((uint32_t)(uint8_t)ctx->rssi_dbm << 8);
        seed ^= (uint32_t)(uint8_t)ctx->snr_db;
    }

    uint16_t jitter = 0u;
    if (policy->jitter_ms > 0u) {
        jitter = (uint16_t)(rml_mix32(seed) % ((uint32_t)policy->jitter_ms + 1u));
    }

    uint16_t delay = (uint16_t)(policy->base_delay_ms + jitter);
    if (effective_priority >= 8u) {
        delay = (uint16_t)(delay / 4u);
    } else if (effective_priority >= 6u) {
        delay = (uint16_t)(delay / 2u);
    }
    return delay;
}

static uint8_t rml_queue_pressure(const rml_context_t *ctx)
{
    if (!ctx) {
        return 0u;
    }
    if (ctx->queue_pressure != 0u) {
        return ctx->queue_pressure > 100u ? 100u : ctx->queue_pressure;
    }
    if (ctx->queue_capacity == 0u) {
        return 0u;
    }
    return (uint8_t)(((uint16_t)ctx->queue_depth * 100u) / ctx->queue_capacity);
}

static bool rml_queue_full(const rml_context_t *ctx)
{
    return ctx && ctx->queue_capacity != 0u && ctx->queue_depth >= ctx->queue_capacity;
}

static uint8_t rml_effective_priority(const rml_message_t *msg,
                                      const rml_context_t *ctx,
                                      const rml_intent_policy_t *policy)
{
    uint8_t priority = msg->priority > policy->min_priority ? msg->priority : policy->min_priority;
    if (msg->intent == RML_INTENT_EMERGENCY) {
        return 255u;
    }
    if (ctx && msg->created_ms != 0u && ctx->now_ms > msg->created_ms) {
        uint32_t age_min = (ctx->now_ms - msg->created_ms) / 60000u;
        if (age_min >= priority) {
            return 0u;
        }
        priority = (uint8_t)(priority - (uint8_t)age_min);
    }
    return priority;
}

static bool rml_airtime_allowed(const rml_context_t *ctx,
                                const rml_intent_policy_t *policy)
{
    if (!ctx || ctx->duty_budget_us == 0u || policy->airtime_floor_pct == 0u) {
        return true;
    }
    uint32_t remaining_pct = (ctx->duty_remaining_us * 100u) / ctx->duty_budget_us;
    return remaining_pct >= policy->airtime_floor_pct;
}

static void rml_thread_expire(uint32_t now_ms)
{
    if (now_ms == 0u) {
        return;
    }
    for (uint16_t i = 0u; i < (uint16_t)RML_THREAD_CACHE_SIZE; i++) {
        rml_thread_entry_t *entry = &s_thread_cache.entries[i];
        if (entry->used && entry->soft_expire_ms != 0u && now_ms >= entry->soft_expire_ms) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static bool rml_status_is_current_or_newer(const rml_message_t *msg)
{
    uint16_t empty = UINT16_MAX;
    for (uint16_t i = 0u; i < (uint16_t)RML_STATUS_CACHE_SIZE; i++) {
        if (s_status[i].used &&
            s_status[i].sender_id == msg->sender_id &&
            s_status[i].state_type == msg->state_type) {
            if (msg->msg_id <= s_status[i].newest_msg_id) {
                return false;
            }
            s_status[i].newest_msg_id = msg->msg_id;
            return true;
        }
        if (!s_status[i].used && empty == UINT16_MAX) {
            empty = i;
        }
    }

    uint16_t slot = empty != UINT16_MAX ? empty : (uint16_t)(msg->sender_id % (uint16_t)RML_STATUS_CACHE_SIZE);
    s_status[slot].used = true;
    s_status[slot].sender_id = msg->sender_id;
    s_status[slot].state_type = msg->state_type;
    s_status[slot].newest_msg_id = msg->msg_id;
    return true;
}

bool rml_thread_cache_get(uint16_t sender_id,
                          uint32_t thread_id,
                          rml_thread_entry_t *out)
{
    const rml_thread_entry_t *best = NULL;
    for (uint16_t i = 0u; i < (uint16_t)RML_THREAD_CACHE_SIZE; i++) {
        const rml_thread_entry_t *entry = &s_thread_cache.entries[i];
        if (entry->used && entry->sender_id == sender_id && entry->thread_id == thread_id) {
            if (!best || entry->timestamp_ms >= best->timestamp_ms) {
                best = entry;
            }
        }
    }
    if (!best) {
        return false;
    }
    if (out) {
        *out = *best;
    }
    return true;
}

static rml_thread_entry_t *rml_thread_find_msg(uint16_t sender_id,
                                               uint32_t thread_id,
                                               uint32_t msg_id)
{
    for (uint16_t i = 0u; i < (uint16_t)RML_THREAD_CACHE_SIZE; i++) {
        rml_thread_entry_t *entry = &s_thread_cache.entries[i];
        if (entry->used &&
            entry->sender_id == sender_id &&
            entry->thread_id == thread_id &&
            entry->msg_id == msg_id) {
            return entry;
        }
    }
    return NULL;
}

static bool rml_thread_store(const rml_message_t *msg,
                             const uint8_t *payload,
                             uint16_t payload_len,
                             uint32_t now_ms)
{
    if (!msg || (!payload && payload_len > 0u) || payload_len > RML_THREAD_TEXT_MAX) {
        return false;
    }
    rml_thread_entry_t *entry = rml_thread_find_msg(msg->sender_id, msg->thread_id, msg->msg_id);
    if (!entry) {
        if (msg->supersedes) {
            for (uint16_t i = 0u; i < (uint16_t)RML_THREAD_CACHE_SIZE; i++) {
                rml_thread_entry_t *old = &s_thread_cache.entries[i];
                if (old->used && old->sender_id == msg->sender_id && old->thread_id == msg->thread_id) {
                    memset(old, 0, sizeof(*old));
                }
            }
        }
        entry = &s_thread_cache.entries[s_thread_cache.next_slot];
        s_thread_cache.next_slot = (uint16_t)((s_thread_cache.next_slot + 1u) % (uint16_t)RML_THREAD_CACHE_SIZE);
    }
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->sender_id = msg->sender_id;
    entry->thread_id = msg->thread_id;
    entry->msg_id = msg->msg_id;
    entry->prev_id = msg->prev_id;
    entry->timestamp_ms = now_ms;
    entry->soft_expire_ms = msg->soft_ttl_s == 0u ? 0u : now_ms + ((uint32_t)msg->soft_ttl_s * 1000u);
    entry->payload_len = payload_len;
    if (payload_len > 0u) {
        memcpy(entry->payload, payload, payload_len);
    }
    return true;
}

static bool rml_apply_delta(const rml_thread_entry_t *base,
                            const rml_delta_t *delta,
                            uint8_t *out,
                            uint16_t *out_len)
{
    if (!base || !delta || !out || !out_len) {
        return false;
    }
    *out_len = 0u;
    if (delta->data_len > RML_MAX_PAYLOAD || delta->offset > base->payload_len) {
        return false;
    }

    if (delta->op == RML_DELTA_APPEND) {
        if ((uint32_t)base->payload_len + delta->data_len > RML_THREAD_TEXT_MAX) {
            return false;
        }
        memcpy(out, base->payload, base->payload_len);
        if (delta->data_len > 0u) {
            memcpy(out + base->payload_len, delta->data, delta->data_len);
        }
        *out_len = (uint16_t)(base->payload_len + delta->data_len);
        return true;
    }

    if (delta->op == RML_DELTA_REPLACE) {
        if ((uint32_t)delta->offset + delta->delete_len > base->payload_len) {
            return false;
        }
        uint16_t tail_off = (uint16_t)(delta->offset + delta->delete_len);
        uint16_t tail_len = (uint16_t)(base->payload_len - tail_off);
        uint32_t new_len = (uint32_t)delta->offset + delta->data_len + tail_len;
        if (new_len > RML_THREAD_TEXT_MAX) {
            return false;
        }
        if (delta->offset > 0u) {
            memcpy(out, base->payload, delta->offset);
        }
        if (delta->data_len > 0u) {
            memcpy(out + delta->offset, delta->data, delta->data_len);
        }
        if (tail_len > 0u) {
            memcpy(out + delta->offset + delta->data_len, base->payload + tail_off, tail_len);
        }
        *out_len = (uint16_t)new_len;
        return true;
    }

    if (delta->op == RML_DELTA_DELETE_RANGE) {
        if ((uint32_t)delta->offset + delta->delete_len > base->payload_len || delta->data_len != 0u) {
            return false;
        }
        uint16_t tail_off = (uint16_t)(delta->offset + delta->delete_len);
        uint16_t tail_len = (uint16_t)(base->payload_len - tail_off);
        if (delta->offset > 0u) {
            memcpy(out, base->payload, delta->offset);
        }
        if (tail_len > 0u) {
            memcpy(out + delta->offset, base->payload + tail_off, tail_len);
        }
        *out_len = (uint16_t)(delta->offset + tail_len);
        return true;
    }

    return false;
}

static rml_policy_decision_t rml_handle_thread_state(const rml_message_t *msg,
                                                     const rml_context_t *ctx)
{
    uint32_t now_ms = ctx ? ctx->now_ms : 0u;
    rml_thread_expire(now_ms);

    if (msg->type == RML_TYPE_CHAT || msg->type == RML_TYPE_STATUS || msg->type == RML_TYPE_THREAD_SYNC) {
        if (!rml_thread_store(msg, msg->payload, msg->payload_len, now_ms)) {
            return rml_decision(false, false, 0u, 0u, RML_REASON_DELTA_FAILED);
        }
        return rml_decision(true, false, 0u, 0u, RML_REASON_OK);
    }

    if (msg->type == RML_TYPE_DELTA) {
        if (msg->prev_id == 0u) {
            return rml_repair_decision(0u, msg->thread_id, msg->sender_id, RML_REASON_MISSING_PREV);
        }
        rml_thread_entry_t *base = rml_thread_find_msg(msg->sender_id, msg->thread_id, msg->prev_id);
        if (!base) {
            return rml_repair_decision(msg->prev_id, msg->thread_id, msg->sender_id, RML_REASON_REPAIR_NEEDED);
        }
        rml_delta_t delta;
        uint8_t rebuilt[RML_THREAD_TEXT_MAX];
        uint16_t rebuilt_len = 0u;
        if (!rml_delta_payload_decode(msg->payload, msg->payload_len, &delta) ||
            !rml_apply_delta(base, &delta, rebuilt, &rebuilt_len) ||
            !rml_thread_store(msg, rebuilt, rebuilt_len, now_ms)) {
            return rml_decision(false, false, 0u, 0u, RML_REASON_DELTA_FAILED);
        }
        rml_policy_decision_t d = rml_decision(true, false, 0u, 0u, RML_REASON_DELTA_APPLIED);
        d.forward_reconstructed = true;
        return d;
    }

    return rml_decision(true, false, 0u, 0u, RML_REASON_OK);
}

rml_policy_decision_t rml_policy_decide(const rml_message_t *msg,
                                        const rml_context_t *ctx)
{
    if (!msg) {
        return rml_decision(false, false, 0u, 0u, RML_REASON_EXPIRED);
    }
    if (msg->magic != RML_MAGIC) {
        g_rml_stats.rml_rx_bad_magic++;
        return rml_decision(false, false, 0u, 0u, RML_REASON_EXPIRED);
    }
    if (msg->version != RML_VERSION) {
        g_rml_stats.rml_rx_bad_version++;
        return rml_decision(false, false, 0u, 0u, RML_REASON_EXPIRED);
    }
    if (msg->payload_len > RML_MAX_PAYLOAD) {
        g_rml_stats.rml_rx_too_large++;
        return rml_decision(false, false, 0u, 0u, RML_REASON_EXPIRED);
    }
    if (rml_seen(msg->msg_id)) {
        g_rml_stats.rml_rx_seen_drop++;
        RIVR_LOGI(TAG, "drop seen msg=%08lx", (unsigned long)msg->msg_id);
        return rml_decision(false, false, 0u, 0u, RML_REASON_SEEN);
    }
    if (ctx && ctx->has_time && msg->expires_s != 0u && ctx->now_s > msg->expires_s) {
        g_rml_stats.rml_rx_expired_drop++;
        return rml_decision(false, false, 0u, 0u, RML_REASON_EXPIRED);
    }
    if (msg->intent == RML_INTENT_STATUS && !rml_status_is_current_or_newer(msg)) {
        return rml_decision(false, false, 0u, 0u, RML_REASON_REPLACED_BY_NEWER_STATUS);
    }

    const rml_intent_policy_t *policy = rml_policy_for_intent(msg->intent);
    uint8_t effective_priority = rml_effective_priority(msg, ctx, policy);

    uint8_t pressure = rml_queue_pressure(ctx);
    if (policy->drop_under_pressure && pressure >= 85u && effective_priority < 6u) {
        return rml_decision(false, false, 0u, 0u, RML_REASON_QUEUE_FULL);
    }

    rml_policy_decision_t state = rml_handle_thread_state(msg, ctx);
    if (!state.accept) {
        return state;
    }
    uint8_t success_reason = state.reason == RML_REASON_DELTA_APPLIED
                           ? RML_REASON_DELTA_APPLIED
                           : RML_REASON_OK;

    rml_mark_seen(msg->msg_id);
    g_rml_stats.rml_rx_total++;

    if (msg->ttl == 0u) {
        g_rml_stats.rml_relay_ttl_drop++;
        return rml_decision(true, false, 0u, 0u, RML_REASON_TTL_ZERO);
    }

    if (ctx && (!ctx->hard_radio_ok || ctx->duty_cycle_blocked || !rml_airtime_allowed(ctx, policy))) {
        return rml_decision(true, false, 0u, 0u, RML_REASON_DUTY_BLOCKED);
    }

    if (rml_queue_full(ctx)) {
        return rml_decision(true, false, 0u, 0u, RML_REASON_QUEUE_FULL);
    }

    bool relay = policy->relay_by_default;
    if (ctx && ctx->local_id != 0u && msg->target_id == ctx->local_id) {
        relay = false;
    }
    if (msg->intent == RML_INTENT_PRIVATE) {
        relay = ctx && ctx->route_hint_valid && ctx->target_hint_useful;
    }
    if (msg->intent == RML_INTENT_EMERGENCY) {
        relay = true;
    }
    if (state.forward_reconstructed) {
        relay = true;
    }
    if (effective_priority == 0u) {
        relay = false;
    }

    uint8_t next_ttl = (uint8_t)(msg->ttl - 1u);
    if (next_ttl > policy->max_relay_ttl) {
        next_ttl = policy->max_relay_ttl;
    }
    if (next_ttl == 0u) {
        relay = false;
    }

    uint16_t delay = relay ? rml_delay_for(msg, ctx, policy, effective_priority) : 0u;
    bool forward_reconstructed = relay && state.forward_reconstructed;
    if (relay) {
        g_rml_stats.rml_relay_total++;
    }

    RIVR_LOGI(TAG, "policy msg=%08lx intent=%u accept=%u relay=%u ttl=%u delay=%u reason=%u",
              (unsigned long)msg->msg_id,
              (unsigned)msg->intent,
              1u,
              relay ? 1u : 0u,
              (unsigned)next_ttl,
              (unsigned)delay,
              (unsigned)success_reason);
    rml_policy_decision_t decision = rml_decision(true, relay, delay, next_ttl, success_reason);
    decision.forward_reconstructed = forward_reconstructed;
    return decision;
}

#endif /* RIVR_ENABLE_RML */
