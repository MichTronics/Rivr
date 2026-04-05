#ifndef RIVR_BLE_COMPANION_H
#define RIVR_BLE_COMPANION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "rivr_config.h"

#if RIVR_FEATURE_BLE

bool rivr_ble_companion_handle_rx(const uint8_t *data, uint16_t len);
void rivr_ble_companion_tick(void);
void rivr_ble_companion_on_disconnect(void);
bool rivr_ble_companion_raw_bridge_enabled(void);
void rivr_ble_companion_push_chat(uint32_t src_id,
                                  const uint8_t *text,
                                  uint8_t text_len);
void rivr_ble_companion_push_node(uint32_t node_id,
                                  const char *callsign,
                                  int8_t rssi_dbm,
                                  int8_t snr_db,
                                  uint8_t hop_count,
                                  uint8_t link_score,
                                  uint8_t role);
void rivr_ble_companion_push_gps_update(uint32_t node_id,
                                        int32_t lat_e5,
                                        int32_t lon_e5,
                                        uint8_t mobility);

#else

static inline bool rivr_ble_companion_handle_rx(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    return false;
}

static inline void rivr_ble_companion_tick(void) {}
static inline void rivr_ble_companion_on_disconnect(void) {}
static inline bool rivr_ble_companion_raw_bridge_enabled(void) { return true; }
static inline void rivr_ble_companion_push_chat(uint32_t src_id,
                                                const uint8_t *text,
                                                uint8_t text_len)
{
    (void)src_id;
    (void)text;
    (void)text_len;
}
static inline void rivr_ble_companion_push_node(uint32_t node_id,
                                                const char *callsign,
                                                int8_t rssi_dbm,
                                                int8_t snr_db,
                                                uint8_t hop_count,
                                                uint8_t link_score,
                                                uint8_t role)
{
    (void)node_id;
    (void)callsign;
    (void)rssi_dbm;
    (void)snr_db;
    (void)hop_count;
    (void)link_score;
    (void)role;
}
static inline void rivr_ble_companion_push_gps_update(uint32_t node_id,
                                                      int32_t lat_e5,
                                                      int32_t lon_e5,
                                                      uint8_t mobility)
{
    (void)node_id;
    (void)lat_e5;
    (void)lon_e5;
    (void)mobility;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
