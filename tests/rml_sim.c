#include "rml_sim.h"

#include <string.h>

#define RML_SIM_NO_NODE 0xFFu

typedef struct {
    rml_message_t msg;
    uint8_t from;
    uint8_t previous;
    bool relay_tx;
    bool used;
} rml_sim_frame_t;

typedef struct {
    bool used;
    uint16_t sender_id;
    uint16_t state_type;
    uint32_t newest_msg_id;
} rml_sim_status_t;

typedef struct {
    bool used;
    uint32_t missing_msg_id;
    uint32_t thread_id;
    uint16_t sender_id;
} rml_sim_repair_t;

typedef struct {
    uint32_t seen[RML_SIM_NODE_SEEN];
    uint16_t seen_next;
    rml_sim_status_t status[RML_SIM_NODE_STATUS];
    rml_thread_entry_t threads[RML_SIM_NODE_THREADS];
    uint16_t thread_next;
    rml_sim_repair_t repairs[RML_SIM_NODE_REPAIRS];
} rml_sim_node_t;

typedef struct {
    rml_sim_frame_t frames[RML_SIM_MAX_FRAMES];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} rml_sim_queue_t;

static uint32_t rml_sim_mix32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

uint32_t rml_estimate_airtime_us(uint16_t payload_len,
                                 uint8_t sf,
                                 uint32_t bw,
                                 uint8_t cr)
{
    uint32_t bw_khz = bw >= 1000u ? (bw / 1000u) : bw;
    if (bw_khz == 0u) {
        bw_khz = 1u;
    }
    if (sf < 6u) {
        sf = 6u;
    }
    if (cr == 0u) {
        cr = 1u;
    }

    uint32_t bytes = (uint32_t)RML_WIRE_HDR_LEN + (uint32_t)payload_len;
    uint32_t symbol_us = ((uint32_t)1u << sf) * 1000u / bw_khz;
    uint32_t payload_symbols = 8u + ((bytes * (uint32_t)(cr + 4u) + 5u) / 6u);
    return (12u + payload_symbols) * symbol_us;
}

void rml_sim_init(rml_sim_t *sim, uint8_t node_count)
{
    if (!sim) {
        return;
    }
    memset(sim, 0, sizeof(*sim));
    sim->cfg.node_count = node_count > RML_SIM_MAX_NODES ? RML_SIM_MAX_NODES : node_count;
    sim->cfg.sf = 7u;
    sim->cfg.bw = 125000u;
    sim->cfg.cr = 1u;
    sim->cfg.now_s = 100u;
    sim->cfg.now_ms = 100000u;
    sim->cfg.duty_budget_us = 1000000u;
    sim->cfg.duty_remaining_us = 1000000u;
    rml_sim_topology_line(sim);
}

void rml_sim_topology_line(rml_sim_t *sim)
{
    if (!sim) {
        return;
    }
    memset(sim->cfg.topology, 0, sizeof(sim->cfg.topology));
    for (uint8_t i = 0u; i + 1u < sim->cfg.node_count; i++) {
        sim->cfg.topology[i][i + 1u] = true;
        sim->cfg.topology[i + 1u][i] = true;
    }
}

void rml_sim_topology_full(rml_sim_t *sim)
{
    if (!sim) {
        return;
    }
    memset(sim->cfg.topology, 0, sizeof(sim->cfg.topology));
    for (uint8_t i = 0u; i < sim->cfg.node_count; i++) {
        for (uint8_t j = 0u; j < sim->cfg.node_count; j++) {
            sim->cfg.topology[i][j] = i != j;
        }
    }
}

void rml_sim_set_loss(rml_sim_t *sim, uint8_t loss_pct)
{
    if (sim) {
        sim->cfg.loss_pct = loss_pct > 100u ? 100u : loss_pct;
    }
}

void rml_sim_set_duty(rml_sim_t *sim,
                      bool constrained,
                      uint32_t budget_us,
                      uint32_t remaining_us)
{
    if (!sim) {
        return;
    }
    sim->cfg.duty_constrained = constrained;
    sim->cfg.duty_budget_us = budget_us;
    sim->cfg.duty_remaining_us = remaining_us;
}

static void rml_sim_metrics_clear(rml_sim_metrics_t *m)
{
    if (m) {
        memset(m, 0, sizeof(*m));
    }
}

static bool rml_sim_queue_push(rml_sim_queue_t *q, const rml_sim_frame_t *frame)
{
    if (!q || !frame || q->count >= RML_SIM_MAX_FRAMES) {
        return false;
    }
    q->frames[q->tail] = *frame;
    q->frames[q->tail].used = true;
    q->tail = (uint16_t)((q->tail + 1u) % (uint16_t)RML_SIM_MAX_FRAMES);
    q->count++;
    return true;
}

static bool rml_sim_queue_pop(rml_sim_queue_t *q, rml_sim_frame_t *frame)
{
    if (!q || !frame || q->count == 0u) {
        return false;
    }
    *frame = q->frames[q->head];
    memset(&q->frames[q->head], 0, sizeof(q->frames[q->head]));
    q->head = (uint16_t)((q->head + 1u) % (uint16_t)RML_SIM_MAX_FRAMES);
    q->count--;
    return true;
}

static void rml_sim_queue_remove_thread(rml_sim_queue_t *q,
                                        uint16_t sender_id,
                                        uint32_t thread_id,
                                        uint8_t keep_type)
{
    if (!q) {
        return;
    }
    for (uint16_t i = 0u; i < (uint16_t)RML_SIM_MAX_FRAMES; i++) {
        rml_sim_frame_t *frame = &q->frames[i];
        if (frame->used && frame->msg.sender_id == sender_id && frame->msg.thread_id == thread_id &&
            (keep_type == 0u || frame->msg.type == keep_type)) {
            memset(frame, 0, sizeof(*frame));
        }
    }
}

static bool rml_sim_seen(const rml_sim_node_t *node, uint32_t msg_id)
{
    if (!node || msg_id == 0u) {
        return false;
    }
    for (uint16_t i = 0u; i < (uint16_t)RML_SIM_NODE_SEEN; i++) {
        if (node->seen[i] == msg_id) {
            return true;
        }
    }
    return false;
}

static void rml_sim_mark_seen(rml_sim_node_t *node, uint32_t msg_id)
{
    if (!node || msg_id == 0u || rml_sim_seen(node, msg_id)) {
        return;
    }
    node->seen[node->seen_next] = msg_id;
    node->seen_next = (uint16_t)((node->seen_next + 1u) % (uint16_t)RML_SIM_NODE_SEEN);
}

static bool rml_sim_status_accept(rml_sim_node_t *node, const rml_message_t *msg)
{
    uint16_t empty = UINT16_MAX;
    for (uint16_t i = 0u; i < (uint16_t)RML_SIM_NODE_STATUS; i++) {
        rml_sim_status_t *slot = &node->status[i];
        if (slot->used && slot->sender_id == msg->sender_id && slot->state_type == msg->state_type) {
            if (msg->msg_id <= slot->newest_msg_id) {
                return false;
            }
            slot->newest_msg_id = msg->msg_id;
            return true;
        }
        if (!slot->used && empty == UINT16_MAX) {
            empty = i;
        }
    }
    uint16_t slot_i = empty != UINT16_MAX ? empty : (uint16_t)(msg->sender_id % (uint16_t)RML_SIM_NODE_STATUS);
    node->status[slot_i].used = true;
    node->status[slot_i].sender_id = msg->sender_id;
    node->status[slot_i].state_type = msg->state_type;
    node->status[slot_i].newest_msg_id = msg->msg_id;
    return true;
}

static rml_thread_entry_t *rml_sim_find_thread(rml_sim_node_t *node,
                                               uint16_t sender_id,
                                               uint32_t thread_id,
                                               uint32_t msg_id)
{
    for (uint16_t i = 0u; i < (uint16_t)RML_SIM_NODE_THREADS; i++) {
        rml_thread_entry_t *entry = &node->threads[i];
        if (entry->used && entry->sender_id == sender_id && entry->thread_id == thread_id &&
            entry->msg_id == msg_id) {
            return entry;
        }
    }
    return NULL;
}

static void rml_sim_clear_thread(rml_sim_node_t *node, uint16_t sender_id, uint32_t thread_id)
{
    for (uint16_t i = 0u; i < (uint16_t)RML_SIM_NODE_THREADS; i++) {
        rml_thread_entry_t *entry = &node->threads[i];
        if (entry->used && entry->sender_id == sender_id && entry->thread_id == thread_id) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static bool rml_sim_store_thread(rml_sim_node_t *node,
                                 const rml_message_t *msg,
                                 const uint8_t *payload,
                                 uint16_t payload_len,
                                 uint32_t now_ms)
{
    if (!node || !msg || (!payload && payload_len > 0u) || payload_len > RML_THREAD_TEXT_MAX) {
        return false;
    }
    if (msg->supersedes) {
        rml_sim_clear_thread(node, msg->sender_id, msg->thread_id);
    }
    rml_thread_entry_t *entry = rml_sim_find_thread(node, msg->sender_id, msg->thread_id, msg->msg_id);
    if (!entry) {
        entry = &node->threads[node->thread_next];
        node->thread_next = (uint16_t)((node->thread_next + 1u) % (uint16_t)RML_SIM_NODE_THREADS);
    }
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->sender_id = msg->sender_id;
    entry->thread_id = msg->thread_id;
    entry->msg_id = msg->msg_id;
    entry->prev_id = msg->prev_id;
    entry->timestamp_ms = now_ms;
    entry->payload_len = payload_len;
    if (payload_len > 0u) {
        memcpy(entry->payload, payload, payload_len);
    }
    return true;
}

static bool rml_sim_apply_delta(const rml_thread_entry_t *base,
                                const rml_delta_t *delta,
                                uint8_t *out,
                                uint16_t *out_len)
{
    if (!base || !delta || !out || !out_len || delta->offset > base->payload_len) {
        return false;
    }
    *out_len = 0u;
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
    return false;
}

static bool rml_sim_repair_once(rml_sim_node_t *node,
                                const rml_message_t *msg,
                                rml_sim_metrics_t *metrics)
{
    metrics->repair_needed++;
    for (uint16_t i = 0u; i < (uint16_t)RML_SIM_NODE_REPAIRS; i++) {
        rml_sim_repair_t *repair = &node->repairs[i];
        if (repair->used && repair->missing_msg_id == msg->prev_id &&
            repair->thread_id == msg->thread_id && repair->sender_id == msg->sender_id) {
            return false;
        }
    }
    uint16_t slot = (uint16_t)(metrics->repair_sent % (uint32_t)RML_SIM_NODE_REPAIRS);
    node->repairs[slot].used = true;
    node->repairs[slot].missing_msg_id = msg->prev_id;
    node->repairs[slot].thread_id = msg->thread_id;
    node->repairs[slot].sender_id = msg->sender_id;
    metrics->repair_sent++;
    return true;
}

static uint8_t rml_sim_airtime_floor(uint8_t intent)
{
    if (intent == RML_INTENT_STATUS) {
        return 30u;
    }
    if (intent == RML_INTENT_TELEMETRY) {
        return 35u;
    }
    if (intent == RML_INTENT_EMERGENCY) {
        return 1u;
    }
    return 15u;
}

static bool rml_sim_duty_allows(const rml_sim_t *sim, uint32_t remaining_us, uint8_t intent)
{
    if (!sim->cfg.duty_constrained || sim->cfg.duty_budget_us == 0u) {
        return true;
    }
    uint32_t pct = (remaining_us * 100u) / sim->cfg.duty_budget_us;
    return pct >= rml_sim_airtime_floor(intent);
}

static bool rml_sim_loss_drops(const rml_sim_t *sim,
                               const rml_message_t *msg,
                               uint8_t from,
                               uint8_t to)
{
    if (sim->cfg.loss_pct == 0u) {
        return false;
    }
    uint32_t seed = msg->msg_id ^ (msg->thread_id * 2654435761u)
                  ^ ((uint32_t)from << 24) ^ ((uint32_t)to << 16)
                  ^ ((uint32_t)msg->type << 8) ^ (uint32_t)msg->payload_len;
    return (rml_sim_mix32(seed) % 100u) < sim->cfg.loss_pct;
}

static bool rml_sim_process_naive(rml_sim_node_t *node,
                                  const rml_message_t *msg,
                                  const rml_sim_t *sim,
                                  rml_sim_metrics_t *metrics,
                                  rml_message_t *out)
{
    if (rml_sim_seen(node, msg->msg_id)) {
        metrics->dropped_seen++;
        return false;
    }
    if (msg->expires_s != 0u && sim->cfg.now_s > msg->expires_s) {
        metrics->dropped_expired++;
        return false;
    }
    rml_sim_mark_seen(node, msg->msg_id);
    *out = *msg;
    if (out->ttl > 0u) {
        out->ttl--;
    }
    return out->ttl > 0u;
}

static bool rml_sim_process_rml(rml_sim_node_t *node,
                                const rml_message_t *msg,
                                const rml_sim_t *sim,
                                uint32_t node_remaining_us,
                                rml_sim_metrics_t *metrics,
                                rml_message_t *out)
{
    if (rml_sim_seen(node, msg->msg_id)) {
        metrics->dropped_seen++;
        return false;
    }
    if (msg->expires_s != 0u && sim->cfg.now_s > msg->expires_s) {
        metrics->dropped_expired++;
        return false;
    }
    if (msg->type == RML_TYPE_STATUS && !rml_sim_status_accept(node, msg)) {
        return false;
    }

    *out = *msg;
    if (msg->type == RML_TYPE_CHAT || msg->type == RML_TYPE_STATUS || msg->type == RML_TYPE_THREAD_SYNC) {
        if (!rml_sim_store_thread(node, msg, msg->payload, msg->payload_len, sim->cfg.now_ms)) {
            return false;
        }
    } else if (msg->type == RML_TYPE_DELTA) {
        rml_thread_entry_t *base = rml_sim_find_thread(node, msg->sender_id, msg->thread_id, msg->prev_id);
        if (!base) {
            rml_sim_repair_once(node, msg, metrics);
            return false;
        }
        rml_delta_t delta;
        uint8_t rebuilt[RML_THREAD_TEXT_MAX];
        uint16_t rebuilt_len = 0u;
        if (!rml_delta_payload_decode(msg->payload, msg->payload_len, &delta) ||
            !rml_sim_apply_delta(base, &delta, rebuilt, &rebuilt_len) ||
            !rml_sim_store_thread(node, msg, rebuilt, rebuilt_len, sim->cfg.now_ms)) {
            return false;
        }
        metrics->delta_applied++;
        out->type = RML_TYPE_THREAD_SYNC;
        out->supersedes = true;
        out->flags |= RML_FLAG_SUPERSEDES;
        out->payload_len = rebuilt_len;
        if (rebuilt_len > 0u) {
            memcpy(out->payload, rebuilt, rebuilt_len);
        }
        metrics->reconstructed_forward++;
    }

    rml_sim_mark_seen(node, msg->msg_id);
    if (out->ttl > 0u) {
        out->ttl--;
    }
    if (out->ttl == 0u) {
        return false;
    }
    if (!rml_sim_duty_allows(sim, node_remaining_us, msg->intent)) {
        metrics->dropped_duty++;
        return false;
    }
    return true;
}

static void rml_sim_enqueue_relay(rml_sim_queue_t *queue,
                                  const rml_message_t *msg,
                                  uint8_t from,
                                  uint8_t previous,
                                  bool compress)
{
    rml_sim_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.msg = *msg;
    frame.from = from;
    frame.previous = previous;
    frame.relay_tx = true;
    frame.used = true;
    if (compress && msg->supersedes) {
        rml_sim_queue_remove_thread(queue, msg->sender_id, msg->thread_id, 0u);
    }
    if (compress && msg->type == RML_TYPE_THREAD_SYNC) {
        rml_sim_queue_remove_thread(queue, msg->sender_id, msg->thread_id, RML_TYPE_THREAD_SYNC);
    }
    (void)rml_sim_queue_push(queue, &frame);
}

bool rml_sim_run(rml_sim_t *sim,
                 rml_sim_mode_t mode,
                 const rml_message_t *messages,
                 uint16_t message_count,
                 rml_sim_metrics_t *out)
{
    if (!sim || !messages || !out || sim->cfg.node_count == 0u || message_count > RML_SIM_MAX_MESSAGES) {
        return false;
    }

    rml_sim_metrics_clear(out);
    rml_sim_node_t nodes[RML_SIM_MAX_NODES];
    uint32_t remaining[RML_SIM_MAX_NODES];
    rml_sim_queue_t queue;
    memset(nodes, 0, sizeof(nodes));
    memset(&queue, 0, sizeof(queue));
    for (uint8_t i = 0u; i < sim->cfg.node_count; i++) {
        remaining[i] = sim->cfg.duty_remaining_us;
    }

    for (uint16_t i = 0u; i < message_count; i++) {
        rml_sim_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.msg = messages[i];
        frame.from = 0u;
        frame.previous = RML_SIM_NO_NODE;
        frame.relay_tx = false;
        frame.used = true;
        if (!rml_sim_queue_push(&queue, &frame)) {
            return false;
        }
    }

    rml_sim_frame_t tx;
    while (rml_sim_queue_pop(&queue, &tx)) {
        if (!tx.used || tx.from >= sim->cfg.node_count) {
            continue;
        }
        uint32_t airtime = rml_estimate_airtime_us(tx.msg.payload_len, sim->cfg.sf, sim->cfg.bw, sim->cfg.cr);
        out->tx_total++;
        if (tx.relay_tx) {
            out->relay_total++;
        }
        out->estimated_airtime_us += airtime;
        if (remaining[tx.from] > airtime) {
            remaining[tx.from] -= airtime;
        } else {
            remaining[tx.from] = 0u;
        }

        for (uint8_t to = 0u; to < sim->cfg.node_count; to++) {
            if (!sim->cfg.topology[tx.from][to] || to == tx.previous) {
                continue;
            }
            if (rml_sim_loss_drops(sim, &tx.msg, tx.from, to)) {
                continue;
            }
            out->rx_total++;
            rml_message_t relay;
            bool should_relay = mode == RML_SIM_MODE_NAIVE
                              ? rml_sim_process_naive(&nodes[to], &tx.msg, sim, out, &relay)
                              : rml_sim_process_rml(&nodes[to], &tx.msg, sim, remaining[to], out, &relay);
            if (should_relay) {
                rml_sim_enqueue_relay(&queue, &relay, to, tx.from, mode == RML_SIM_MODE_RML);
            }
        }
    }

    return true;
}