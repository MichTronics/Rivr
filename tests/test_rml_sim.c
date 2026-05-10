#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rml_sim.h"

static uint32_t s_pass;
static uint32_t s_fail;

#define CHECK(cond, msg) do {                                      \
    if (cond) { printf("  OK   %s\n", (msg)); s_pass++; }          \
    else       { printf("FAIL  %s  [%s:%d]\n", (msg),              \
                        __FILE__, __LINE__); s_fail++; }           \
} while (0)

static rml_message_t make_msg(uint32_t msg_id,
                              uint8_t type,
                              uint8_t intent,
                              uint32_t thread_id,
                              const char *text)
{
    rml_message_t msg;
    bool ok = rml_make_chat(0x1201u, 0u, thread_id,
                            (const uint8_t *)text,
                            (uint16_t)strlen(text), &msg);
    CHECK(ok, "message creation");
    msg.msg_id = msg_id;
    msg.type = type;
    msg.intent = intent;
    msg.state_type = (uint16_t)thread_id;
    msg.priority = 3u;
    msg.ttl = 8u;
    return msg;
}

static rml_delta_t make_append_delta(const char *text)
{
    rml_delta_t delta;
    memset(&delta, 0, sizeof(delta));
    delta.op = RML_DELTA_APPEND;
    delta.data_len = (uint16_t)strlen(text);
    memcpy(delta.data, text, delta.data_len);
    return delta;
}

static void make_delta_chain(rml_message_t *messages, uint16_t *count)
{
    *count = 0u;
    messages[(*count)++] = make_msg(0x5000u, RML_TYPE_CHAT, RML_INTENT_CHAT, 0x7000u, "a");
    uint32_t prev = 0x5000u;
    for (uint32_t i = 0u; i < 8u; i++) {
        char text[2] = { (char)('b' + (char)i), '\0' };
        rml_delta_t delta = make_append_delta(text);
        rml_message_t msg;
        CHECK(rml_make_delta(0x1201u, 0u, 0x7000u, 0x5001u + i, prev, &delta, &msg),
              "delta creation");
        msg.ttl = 8u;
        messages[(*count)++] = msg;
        prev = msg.msg_id;
    }
}

static void test_airtime_monotonic(void)
{
    printf("\n=== RML sim airtime estimate ===\n");
    uint32_t small = rml_estimate_airtime_us(10u, 7u, 125000u, 1u);
    uint32_t large = rml_estimate_airtime_us(80u, 7u, 125000u, 1u);
    uint32_t high_sf = rml_estimate_airtime_us(10u, 10u, 125000u, 1u);
    uint32_t low_bw = rml_estimate_airtime_us(10u, 7u, 62500u, 1u);
    CHECK(large > small, "larger payload increases airtime");
    CHECK(high_sf > small, "higher sf increases airtime");
    CHECK(low_bw > small, "lower bandwidth increases airtime");
}

static void test_chat_baseline(void)
{
    printf("\n=== RML sim chat baseline ===\n");
    rml_sim_t sim;
    rml_sim_metrics_t naive;
    rml_sim_metrics_t rml;
    rml_message_t msg = make_msg(0x1001u, RML_TYPE_CHAT, RML_INTENT_CHAT, 0x2001u, "hello");
    rml_sim_init(&sim, 6u);
    rml_sim_topology_full(&sim);
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_NAIVE, &msg, 1u, &naive), "naive chat sim runs");
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_RML, &msg, 1u, &rml), "rml chat sim runs");
    CHECK(naive.tx_total > 0u && rml.tx_total > 0u, "chat flood produces transmissions");
    CHECK(rml.dropped_seen >= naive.dropped_seen, "rml seen cache participates in chat flood");
}

static void test_status_dominance(void)
{
    printf("\n=== RML sim status dominance ===\n");
    rml_message_t messages[6];
    rml_sim_t sim;
    rml_sim_metrics_t naive;
    rml_sim_metrics_t rml;

    for (uint16_t i = 0u; i < 6u; i++) {
        messages[i] = make_msg(0x3006u - i, RML_TYPE_STATUS, RML_INTENT_STATUS, 0x0042u, "s");
        messages[i].state_type = 42u;
    }
    rml_sim_init(&sim, 6u);
    rml_sim_topology_line(&sim);
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_NAIVE, messages, 6u, &naive), "naive status sim runs");
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_RML, messages, 6u, &rml), "rml status sim runs");
    CHECK(rml.relay_total < naive.relay_total, "status dominance relays fewer frames");
    CHECK(rml.estimated_airtime_us < naive.estimated_airtime_us, "status dominance lowers airtime");
}

static void test_delta_thread_compression(void)
{
    printf("\n=== RML sim delta compression ===\n");
    rml_message_t messages[RML_SIM_MAX_MESSAGES];
    uint16_t count;
    rml_sim_t sim;
    rml_sim_metrics_t naive;
    rml_sim_metrics_t rml;

    make_delta_chain(messages, &count);
    rml_sim_init(&sim, 6u);
    rml_sim_topology_line(&sim);
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_NAIVE, messages, count, &naive), "naive delta sim runs");
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_RML, messages, count, &rml), "rml delta sim runs");
    CHECK(rml.delta_applied > 0u, "deltas are applied");
    CHECK(rml.reconstructed_forward > 0u, "reconstructed frames are forwarded");
    CHECK(rml.relay_total < naive.relay_total, "thread compression reduces relays");
    CHECK(rml.estimated_airtime_us < naive.estimated_airtime_us, "thread compression lowers airtime");
}

static void test_missing_prev_repair_bound(void)
{
    printf("\n=== RML sim delayed repair bound ===\n");
    rml_message_t messages[4];
    rml_sim_t sim;
    rml_sim_metrics_t metrics;
    rml_delta_t delta = make_append_delta("x");

    for (uint16_t i = 0u; i < 4u; i++) {
        CHECK(rml_make_delta(0x1201u, 0u, 0x7100u, 0x6000u + i, 0x5F00u, &delta, &messages[i]),
              "missing-prev delta creation");
        messages[i].ttl = 8u;
    }
    rml_sim_init(&sim, 6u);
    rml_sim_topology_full(&sim);
    rml_sim_set_loss(&sim, 30u);
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_RML, messages, 4u, &metrics), "repair sim runs");
    CHECK(metrics.repair_needed > 0u, "missing prev needs repair");
    CHECK(metrics.repair_sent <= metrics.repair_needed, "repair sends are bounded by needs");
    CHECK(metrics.repair_sent <= (uint32_t)RML_SIM_MAX_NODES, "repair sends are bounded per node/key");
}

static void test_supersede_thread(void)
{
    printf("\n=== RML sim supersede thread ===\n");
    rml_message_t messages[6];
    rml_sim_t sim;
    rml_sim_metrics_t naive;
    rml_sim_metrics_t rml;

    for (uint16_t i = 0u; i < 6u; i++) {
        messages[i] = make_msg(0x8000u + i, RML_TYPE_CHAT, RML_INTENT_CHAT, 0x8100u, i == 5u ? "latest" : "old");
        if (i == 5u) {
            messages[i].supersedes = true;
            messages[i].flags |= RML_FLAG_SUPERSEDES;
        }
    }
    rml_sim_init(&sim, 6u);
    rml_sim_topology_line(&sim);
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_NAIVE, messages, 6u, &naive), "naive supersede sim runs");
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_RML, messages, 6u, &rml), "rml supersede sim runs");
    CHECK(rml.relay_total < naive.relay_total, "supersede relays fewer stale frames");
}

static void test_packet_loss_levels(void)
{
    printf("\n=== RML sim packet loss levels ===\n");
    rml_message_t messages[RML_SIM_MAX_MESSAGES];
    uint16_t count;
    uint8_t levels[] = { 10u, 30u, 50u };

    make_delta_chain(messages, &count);
    for (uint16_t i = 0u; i < (uint16_t)(sizeof(levels) / sizeof(levels[0])); i++) {
        rml_sim_t sim;
        rml_sim_metrics_t metrics;
        rml_sim_init(&sim, 6u);
        rml_sim_topology_full(&sim);
        rml_sim_set_loss(&sim, levels[i]);
        CHECK(rml_sim_run(&sim, RML_SIM_MODE_RML, messages, count, &metrics), "loss sim runs");
        CHECK(metrics.repair_sent <= metrics.repair_needed, "loss repair sends remain bounded");
        CHECK(metrics.repair_sent <= (uint32_t)(RML_SIM_MAX_NODES * RML_SIM_NODE_REPAIRS),
              "loss repair sends stay within fixed cache bound");
    }
}

static void test_duty_budget_constrained(void)
{
    printf("\n=== RML sim duty constrained ===\n");
    rml_message_t messages[4];
    rml_sim_t sim;
    rml_sim_metrics_t metrics;

    for (uint16_t i = 0u; i < 4u; i++) {
        messages[i] = make_msg(0x9000u + i, RML_TYPE_STATUS, RML_INTENT_STATUS, 0x9200u + i, "duty");
        messages[i].state_type = (uint16_t)(0x20u + i);
    }
    rml_sim_init(&sim, 6u);
    rml_sim_topology_line(&sim);
    rml_sim_set_duty(&sim, true, 1000000u, 10000u);
    CHECK(rml_sim_run(&sim, RML_SIM_MODE_RML, messages, 4u, &metrics), "duty sim runs");
    CHECK(metrics.dropped_duty > 0u, "duty budget suppresses relays");
}

int main(void)
{
    test_airtime_monotonic();
    test_chat_baseline();
    test_status_dominance();
    test_delta_thread_compression();
    test_missing_prev_repair_bound();
    test_supersede_thread();
    test_packet_loss_levels();
    test_duty_budget_constrained();

    printf("\nRML sim tests: pass=%u fail=%u\n", (unsigned)s_pass, (unsigned)s_fail);
    return s_fail == 0u ? 0 : 1;
}