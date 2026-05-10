#ifndef TESTS_RML_SIM_H
#define TESTS_RML_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "firmware_core/rml/rml.h"

#define RML_SIM_MAX_NODES 8u
#define RML_SIM_MAX_FRAMES 192u
#define RML_SIM_MAX_MESSAGES 32u
#define RML_SIM_NODE_SEEN 64u
#define RML_SIM_NODE_STATUS 16u
#define RML_SIM_NODE_THREADS 16u
#define RML_SIM_NODE_REPAIRS 16u

typedef enum {
    RML_SIM_MODE_NAIVE = 0,
    RML_SIM_MODE_RML = 1,
} rml_sim_mode_t;

typedef struct {
    uint32_t tx_total;
    uint32_t rx_total;
    uint32_t relay_total;
    uint32_t dropped_seen;
    uint32_t dropped_expired;
    uint32_t dropped_duty;
    uint32_t repair_needed;
    uint32_t repair_sent;
    uint32_t delta_applied;
    uint32_t reconstructed_forward;
    uint32_t estimated_airtime_us;
} rml_sim_metrics_t;

typedef struct {
    uint8_t node_count;
    uint8_t loss_pct;
    uint8_t sf;
    uint32_t bw;
    uint8_t cr;
    uint32_t now_s;
    uint32_t now_ms;
    uint32_t duty_budget_us;
    uint32_t duty_remaining_us;
    bool duty_constrained;
    bool topology[RML_SIM_MAX_NODES][RML_SIM_MAX_NODES];
} rml_sim_config_t;

typedef struct {
    rml_sim_config_t cfg;
} rml_sim_t;

uint32_t rml_estimate_airtime_us(uint16_t payload_len,
                                 uint8_t sf,
                                 uint32_t bw,
                                 uint8_t cr);

void rml_sim_init(rml_sim_t *sim, uint8_t node_count);
void rml_sim_topology_line(rml_sim_t *sim);
void rml_sim_topology_full(rml_sim_t *sim);
void rml_sim_set_loss(rml_sim_t *sim, uint8_t loss_pct);
void rml_sim_set_duty(rml_sim_t *sim,
                      bool constrained,
                      uint32_t budget_us,
                      uint32_t remaining_us);

bool rml_sim_run(rml_sim_t *sim,
                 rml_sim_mode_t mode,
                 const rml_message_t *messages,
                 uint16_t message_count,
                 rml_sim_metrics_t *out);

#endif /* TESTS_RML_SIM_H */