#ifndef RIVR_BLE_COMPANION_H
#define RIVR_BLE_COMPANION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "rivr_config.h"
/* rivr_live_stats_t is used in declarations on both sides of RIVR_FEATURE_BLE */
#include "../rivr_metrics.h"

#if RIVR_FEATURE_BLE

/* rivr_metrics.h already included above */

bool rivr_ble_companion_handle_rx(const uint8_t *data, uint16_t len);
void rivr_ble_companion_tick(void);
void rivr_ble_companion_on_disconnect(void);
bool rivr_ble_companion_raw_bridge_enabled(void);

/**
 * @brief Feed a SLIP-decoded CP packet received from UART0 into the shared
 *        command queue, tagged as serial origin so APP_START activates the
 *        serial session (UART0 TX) rather than the BLE session.
 */
bool rivr_serial_cp_handle_rx(const uint8_t *data, uint16_t len);

/**
 * @brief Terminate the active serial/UART0 CP session (call on USB disconnect).
 */
void rivr_serial_cp_session_stop(void);

/**
 * @brief Activate a serial CP session immediately (e.g. via the "appmode"
 *        CLI command).  Sets the serial session active and sends DEVICE_INFO.
 */
void rivr_serial_cp_start_session(void);

/**
 * @brief Returns true while the UART0 serial CP session is active.
 *        Used to suppress duplicate ASCII log lines (@CHT, @MET).
 */
bool rivr_serial_cp_session_active(void);

/**
 * @brief Push a DEVICE_INFO CP packet over the SLIP/UART0 channel.
 *        No-op when the serial session is not active.
 */
void rivr_serial_cp_push_device_info(void);

/**
 * @brief Push a full PKT_METRICS snapshot over the SLIP/UART0 channel.
 *
 * Encodes @p live into the same compact rivr_met_ble_payload_t used by
 * rivr_metrics_ble_push() and wraps it in a RIVR_CP_PKT_METRICS_PUSH (0x8B)
 * CP packet.  The companion app parses it with RivrFrameCodec.parseFrame()
 * — identical to the BLE metrics path.  No-op when the serial session is
 * not active.
 */
void rivr_serial_cp_push_metrics(const rivr_live_stats_t *live,
                                 uint32_t src_id, uint16_t net_id,
                                 uint16_t seq);
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

/**
 * @brief Push an incoming private chat message to the connected companion.
 *
 * CP packet type 0x88 (RIVR_CP_PKT_PRIVATE_CHAT_RX).
 * Payload: [msg_id:8 LE][from_id:4 LE][to_id:4 LE][sender_seq:4 LE]
 *          [timestamp_s:4 LE][flags:2 LE][body_len:1][body:N]
 */
void rivr_ble_companion_push_private_chat_rx(uint64_t msg_id,
                                              uint32_t from_id,
                                              uint32_t to_id,
                                              uint32_t sender_seq,
                                              uint32_t timestamp_s,
                                              uint16_t flags,
                                              const uint8_t *body,
                                              uint8_t body_len);

/**
 * @brief Push a delivery state change for an outgoing private chat message.
 *
 * CP packet type 0x89 (RIVR_CP_PKT_PRIVATE_CHAT_STATE).
 * Payload: [msg_id:8 LE][peer_id:4 LE][state:1]
 */
void rivr_ble_companion_push_pchat_state(uint64_t msg_id,
                                          uint32_t peer_id,
                                          uint8_t state);

/**
 * @brief Push a delivery receipt event to the companion.
 *
 * CP packet type 0x8A (RIVR_CP_PKT_DELIVERY_RECEIPT).
 * Payload: [orig_msg_id:8 LE][sender_id:4 LE][timestamp_s:4 LE][status:1]
 */
void rivr_ble_companion_push_delivery_receipt(uint64_t orig_msg_id,
                                               uint32_t sender_id,
                                               uint32_t timestamp_s,
                                               uint8_t status);

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
static inline void rivr_ble_companion_push_private_chat_rx(uint64_t msg_id,
                                                            uint32_t from_id,
                                                            uint32_t to_id,
                                                            uint32_t sender_seq,
                                                            uint32_t timestamp_s,
                                                            uint16_t flags,
                                                            const uint8_t *body,
                                                            uint8_t body_len)
{
    (void)msg_id; (void)from_id; (void)to_id; (void)sender_seq;
    (void)timestamp_s; (void)flags; (void)body; (void)body_len;
}
static inline void rivr_ble_companion_push_pchat_state(uint64_t msg_id,
                                                        uint32_t peer_id,
                                                        uint8_t state)
{
    (void)msg_id; (void)peer_id; (void)state;
}
static inline void rivr_ble_companion_push_delivery_receipt(uint64_t orig_msg_id,
                                                             uint32_t sender_id,
                                                             uint32_t timestamp_s,
                                                             uint8_t status)
{
    (void)orig_msg_id; (void)sender_id; (void)timestamp_s; (void)status;
}
static inline bool rivr_serial_cp_handle_rx(const uint8_t *data, uint16_t len)
{
    (void)data; (void)len; return false;
}
static inline void rivr_serial_cp_session_stop(void) {}
static inline void rivr_serial_cp_start_session(void) {}
static inline bool rivr_serial_cp_session_active(void) { return false; }
static inline void rivr_serial_cp_push_device_info(void) {}
static inline void rivr_serial_cp_push_metrics(const rivr_live_stats_t *live,
                                                uint32_t src_id,
                                                uint16_t net_id,
                                                uint16_t seq)
{ (void)live; (void)src_id; (void)net_id; (void)seq; }

#endif

#ifdef __cplusplus
}
#endif

#endif
