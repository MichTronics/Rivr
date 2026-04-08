/**
 * @file  rivr_ble_companion.c
 * @brief Rivr BLE companion protocol layered on top of the NUS transport.
 */

#include "rivr_ble_companion.h"

#if RIVR_FEATURE_BLE

#include <stdio.h>
#include <string.h>

#include "../build_info.h"
#include "../private_chat.h"
#include "../radio_sx1262.h"
#include "../routing.h"
#include "../rivr_log.h"
#include "../rivr_metrics.h"
#include "../timebase.h"
#include "rivr_ble.h"
#include "rivr_ble_service.h"
#include "rivr_layer/rivr_embed.h"

#define TAG "RIVR_BLE_CP"

#define RIVR_BLE_CP_MAGIC0 0x52u
#define RIVR_BLE_CP_MAGIC1 0x43u
#define RIVR_BLE_CP_VERSION 1u
#define RIVR_BLE_CP_HDR_LEN 5u
#define RIVR_BLE_CP_QUEUE_LEN 4u
#define RIVR_BLE_CP_MAX_CHAT_LEN 180u

enum {
    RIVR_CP_CMD_APP_START        = 0x01u,
    RIVR_CP_CMD_DEVICE_QUERY     = 0x02u,
    RIVR_CP_CMD_SET_CALLSIGN     = 0x03u,
    RIVR_CP_CMD_GET_NEIGHBORS    = 0x04u,
    RIVR_CP_CMD_SET_POSITION     = 0x05u,
    RIVR_CP_CMD_GET_POSITION     = 0x06u,
    RIVR_CP_CMD_SEND_PRIVATE     = 0x07u,  /**< Send private message */
};

enum {
    RIVR_CP_PKT_OK                   = 0x80u,
    RIVR_CP_PKT_ERR                  = 0x81u,
    RIVR_CP_PKT_DEVICE_INFO          = 0x82u,
    RIVR_CP_PKT_NODE_INFO            = 0x83u,
    RIVR_CP_PKT_NODE_LIST_DONE       = 0x84u,
    RIVR_CP_PKT_CHAT_RX              = 0x85u,
    RIVR_CP_PKT_GPS_UPDATE           = 0x86u,
    RIVR_CP_PKT_DEVICE_POSITION      = 0x87u,
    RIVR_CP_PKT_PRIVATE_CHAT_RX      = 0x88u,  /**< Incoming private message event */
    RIVR_CP_PKT_PRIVATE_CHAT_STATE   = 0x89u,  /**< Outgoing message state update  */
    RIVR_CP_PKT_DELIVERY_RECEIPT     = 0x8Au,  /**< End-to-end delivery receipt     */
};

typedef struct {
    uint8_t len;
    uint8_t data[RF_MAX_PAYLOAD_LEN];
} rivr_ble_companion_packet_t;

static rivr_ble_companion_packet_t s_rx_queue[RIVR_BLE_CP_QUEUE_LEN];
static volatile uint8_t s_rx_head = 0u;
static volatile uint8_t s_rx_tail = 0u;
static volatile bool s_session_active = false;

static bool cp_callsign_valid(const char *callsign)
{
    size_t len = 0u;

    if (!callsign || callsign[0] == '\0') {
        return false;
    }

    while (callsign[len] != '\0') {
        char c = callsign[len];
        bool valid = ((c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') ||
                      c == '-');
        if (!valid || len >= 11u) {
            return false;
        }
        len++;
    }

    return len > 0u && len <= 11u;
}

static bool cp_send_packet(uint8_t type, uint8_t status,
                           const uint8_t *payload, uint8_t payload_len)
{
    uint8_t packet[RF_MAX_PAYLOAD_LEN];
    size_t total_len = RIVR_BLE_CP_HDR_LEN + (size_t)payload_len;

    if (!rivr_ble_is_connected() || !s_session_active) {
        return false;
    }
    if (total_len > RF_MAX_PAYLOAD_LEN) {
        return false;
    }

    packet[0] = RIVR_BLE_CP_MAGIC0;
    packet[1] = RIVR_BLE_CP_MAGIC1;
    packet[2] = RIVR_BLE_CP_VERSION;
    packet[3] = type;
    packet[4] = status;
    if (payload_len != 0u && payload) {
        memcpy(&packet[RIVR_BLE_CP_HDR_LEN], payload, payload_len);
    }

    return rivr_ble_service_notify(rivr_ble_conn_handle(), packet, (uint8_t)total_len);
}

static void cp_send_ok(uint8_t cmd)
{
    (void)cp_send_packet(RIVR_CP_PKT_OK, 0u, &cmd, 1u);
}

static void cp_send_err(uint8_t cmd, const char *msg)
{
    uint8_t payload[49];
    size_t msg_len = 0u;

    payload[0] = cmd;
    if (msg) {
        msg_len = strlen(msg);
        if (msg_len > sizeof(payload) - 1u) {
            msg_len = sizeof(payload) - 1u;
        }
        memcpy(&payload[1], msg, msg_len);
    }

    (void)cp_send_packet(RIVR_CP_PKT_ERR, 1u, payload, (uint8_t)(1u + msg_len));
}

static void cp_send_device_info(void)
{
    char payload[192];
    int n = snprintf(payload, sizeof(payload),
                     "{\"node_id\":\"0x%08lx\",\"callsign\":\"%s\","
                     "\"net_id\":\"0x%04X\",\"env\":\"%s\","
                     "\"role\":\"%s\",\"radio\":\"%s\"}",
                     (unsigned long)g_my_node_id,
                     g_callsign,
                     (unsigned)g_net_id,
                     RIVR_BUILD_ENV,
#if RIVR_ROLE_CLIENT
                     "client",
#elif RIVR_ROLE_REPEATER || (defined(RIVR_BUILD_REPEATER) && RIVR_BUILD_REPEATER)
                     "repeater",
#elif RIVR_ROLE_GATEWAY
                     "gateway",
#else
                     "generic",
#endif
#if defined(RIVR_RADIO_LR1110) && RIVR_RADIO_LR1110
                     "LR1110"
#elif defined(RIVR_RADIO_SX1276) && RIVR_RADIO_SX1276
                     "SX1276"
#else
                     "SX1262"
#endif
    );

    if (n < 0) {
        cp_send_err(RIVR_CP_CMD_DEVICE_QUERY, "device info failed");
        return;
    }
    if (n >= (int)sizeof(payload)) {
        n = (int)(sizeof(payload) - 1u);
    }

    (void)cp_send_packet(RIVR_CP_PKT_DEVICE_INFO, 0u,
                         (const uint8_t *)payload, (uint8_t)n);
}

static void cp_send_node_info(const neighbor_entry_t *entry, uint32_t now_ms)
{
    uint8_t payload[22];
    uint16_t age_s;
    uint8_t score;
    size_t cs_len = 0u;

    if (!entry || entry->node_id == 0u) {
        return;
    }

    age_s = (uint16_t)((now_ms - entry->last_seen_ms) / 1000u);
    score = routing_neighbor_link_score(entry, now_ms);

    payload[0] = (uint8_t)(entry->node_id & 0xFFu);
    payload[1] = (uint8_t)((entry->node_id >> 8) & 0xFFu);
    payload[2] = (uint8_t)((entry->node_id >> 16) & 0xFFu);
    payload[3] = (uint8_t)((entry->node_id >> 24) & 0xFFu);
    payload[4] = (uint8_t)entry->rssi_dbm;
    payload[5] = (uint8_t)entry->last_snr_db;
    payload[6] = entry->hop_count;
    payload[7] = score;
    payload[8] = (uint8_t)(age_s & 0xFFu);
    payload[9] = (uint8_t)((age_s >> 8) & 0xFFu);
    memset(&payload[10], 0, 12u);
    cs_len = strnlen(entry->callsign, 11u);
    memcpy(&payload[10], entry->callsign, cs_len);

    (void)cp_send_packet(RIVR_CP_PKT_NODE_INFO, 0u, payload, sizeof(payload));
}

static void cp_handle_set_callsign(const uint8_t *payload, uint8_t payload_len)
{
    char callsign[12];

    if (!payload || payload_len == 0u || payload_len > 11u) {
        cp_send_err(RIVR_CP_CMD_SET_CALLSIGN, "invalid callsign");
        return;
    }

    memcpy(callsign, payload, payload_len);
    callsign[payload_len] = '\0';
    if (!cp_callsign_valid(callsign)) {
        cp_send_err(RIVR_CP_CMD_SET_CALLSIGN, "invalid callsign");
        return;
    }

    strncpy(g_callsign, callsign, sizeof(g_callsign) - 1u);
    g_callsign[sizeof(g_callsign) - 1u] = '\0';

    if (!rivr_nvs_store_identity(g_callsign, g_net_id)) {
        cp_send_err(RIVR_CP_CMD_SET_CALLSIGN, "persist failed");
        return;
    }

    cp_send_ok(RIVR_CP_CMD_SET_CALLSIGN);
}

static void cp_handle_get_neighbors(void)
{
    uint32_t now_ms = tb_millis();

    for (uint8_t i = 0u; i < NEIGHBOR_TABLE_SIZE; i++) {
        const neighbor_entry_t *entry = routing_neighbor_get(&g_neighbor_table, i);
        if (!entry) {
            continue;
        }
        if ((uint32_t)(now_ms - entry->last_seen_ms) > NEIGHBOR_EXPIRY_MS) {
            continue;
        }
        cp_send_node_info(entry, now_ms);
    }

    (void)cp_send_packet(RIVR_CP_PKT_NODE_LIST_DONE, 0u, NULL, 0u);
}

static void cp_handle_send_private(const uint8_t *payload, uint8_t payload_len)
{
    /* Payload: [dst_id:4 LE][body:N] */
    if (!payload || payload_len < 4u) {
        cp_send_err(RIVR_CP_CMD_SEND_PRIVATE, "payload too short");
        return;
    }

    uint32_t dst_id = (uint32_t)payload[0]
                    | ((uint32_t)payload[1] << 8)
                    | ((uint32_t)payload[2] << 16)
                    | ((uint32_t)payload[3] << 24);

    const uint8_t *body     = &payload[4];
    uint8_t        body_len = (uint8_t)(payload_len - 4u);

    if (body_len > PRIVATE_CHAT_MAX_BODY) {
        cp_send_err(RIVR_CP_CMD_SEND_PRIVATE, "body too long");
        return;
    }

    uint64_t msg_id = 0u;
    pchat_error_t rc = private_chat_send(dst_id, body, body_len, &msg_id);
    if (rc != PCHAT_OK) {
        cp_send_err(RIVR_CP_CMD_SEND_PRIVATE, "send failed");
        return;
    }

    /* Respond with OK + the 8-byte msg_id so the app can track state. */
    uint8_t ok_payload[9];
    ok_payload[0] = RIVR_CP_CMD_SEND_PRIVATE;
    for (uint8_t i = 0u; i < 8u; i++) {
        ok_payload[1u + i] = (uint8_t)((msg_id >> (i * 8u)) & 0xFFu);
    }
    (void)cp_send_packet(RIVR_CP_PKT_OK, 0u, ok_payload, sizeof(ok_payload));
}

bool rivr_ble_companion_handle_rx(const uint8_t *data, uint16_t len)
{
    uint8_t next_head;

    if (!data || len < RIVR_BLE_CP_HDR_LEN || len > RF_MAX_PAYLOAD_LEN) {
        return false;
    }
    if (data[0] != RIVR_BLE_CP_MAGIC0 || data[1] != RIVR_BLE_CP_MAGIC1) {
        return false;
    }
    if (data[2] != RIVR_BLE_CP_VERSION) {
        g_rivr_metrics.ble_errors++;
        return true;
    }

    next_head = (uint8_t)((s_rx_head + 1u) % RIVR_BLE_CP_QUEUE_LEN);
    if (next_head == s_rx_tail) {
        g_rivr_metrics.ble_errors++;
        RIVR_LOGW(TAG, "rx queue full");
        return true;
    }

    memcpy(s_rx_queue[s_rx_head].data, data, len);
    s_rx_queue[s_rx_head].len = (uint8_t)len;
    s_rx_head = next_head;
    g_rivr_metrics.ble_rx_frames++;
    return true;
}

void rivr_ble_companion_tick(void)
{
    while (s_rx_tail != s_rx_head) {
        rivr_ble_companion_packet_t packet = s_rx_queue[s_rx_tail];
        uint8_t cmd = packet.data[3];
        const uint8_t *payload = &packet.data[RIVR_BLE_CP_HDR_LEN];
        uint8_t payload_len = (uint8_t)(packet.len - RIVR_BLE_CP_HDR_LEN);

        s_rx_tail = (uint8_t)((s_rx_tail + 1u) % RIVR_BLE_CP_QUEUE_LEN);

        switch (cmd) {
        case RIVR_CP_CMD_APP_START:
            s_session_active = true;
            cp_send_ok(RIVR_CP_CMD_APP_START);
            break;

        case RIVR_CP_CMD_DEVICE_QUERY:
            if (!s_session_active) {
                cp_send_err(cmd, "app start required");
                break;
            }
            cp_send_device_info();
            break;

        case RIVR_CP_CMD_SET_CALLSIGN:
            if (!s_session_active) {
                cp_send_err(cmd, "app start required");
                break;
            }
            cp_handle_set_callsign(payload, payload_len);
            break;

        case RIVR_CP_CMD_GET_NEIGHBORS:
            if (!s_session_active) {
                cp_send_err(cmd, "app start required");
                break;
            }
            cp_handle_get_neighbors();
            break;

        case RIVR_CP_CMD_SEND_PRIVATE:
            if (!s_session_active) {
                cp_send_err(cmd, "app start required");
                break;
            }
            cp_handle_send_private(payload, payload_len);
            break;

        default:
            if (s_session_active) {
                cp_send_err(cmd, "unknown command");
            }
            break;
        }
    }
}

void rivr_ble_companion_on_disconnect(void)
{
    s_session_active = false;
    s_rx_head = 0u;
    s_rx_tail = 0u;
}

bool rivr_ble_companion_raw_bridge_enabled(void)
{
    return !s_session_active;
}

void rivr_ble_companion_push_chat(uint32_t src_id,
                                  const uint8_t *text,
                                  uint8_t text_len)
{
    uint8_t payload[4u + RIVR_BLE_CP_MAX_CHAT_LEN];
    uint8_t copy_len = text_len;

    if (!s_session_active || !text || text_len == 0u) {
        return;
    }
    if (copy_len > RIVR_BLE_CP_MAX_CHAT_LEN) {
        copy_len = RIVR_BLE_CP_MAX_CHAT_LEN;
    }

    payload[0] = (uint8_t)(src_id & 0xFFu);
    payload[1] = (uint8_t)((src_id >> 8) & 0xFFu);
    payload[2] = (uint8_t)((src_id >> 16) & 0xFFu);
    payload[3] = (uint8_t)((src_id >> 24) & 0xFFu);
    memcpy(&payload[4], text, copy_len);
    (void)cp_send_packet(RIVR_CP_PKT_CHAT_RX, 0u, payload, (uint8_t)(4u + copy_len));
}

void rivr_ble_companion_push_node(uint32_t node_id,
                                  const char *callsign,
                                  int8_t rssi_dbm,
                                  int8_t snr_db,
                                  uint8_t hop_count,
                                  uint8_t link_score,
                                  uint8_t role)
{
    uint8_t payload[22];
    size_t cs_len = 0u;

    if (!s_session_active) {
        return;
    }

    payload[0] = (uint8_t)(node_id & 0xFFu);
    payload[1] = (uint8_t)((node_id >> 8) & 0xFFu);
    payload[2] = (uint8_t)((node_id >> 16) & 0xFFu);
    payload[3] = (uint8_t)((node_id >> 24) & 0xFFu);
    payload[4] = (uint8_t)rssi_dbm;
    payload[5] = (uint8_t)snr_db;
    payload[6] = hop_count;
    payload[7] = link_score;
    payload[8] = role;  /* node role (rivr_node_role_t) */
    payload[9] = 0u;
    memset(&payload[10], 0, 12u);
    if (callsign) {
        cs_len = strnlen(callsign, 11u);
        memcpy(&payload[10], callsign, cs_len);
    }

    (void)cp_send_packet(RIVR_CP_PKT_NODE_INFO, 0u, payload, sizeof(payload));
}

void rivr_ble_companion_push_private_chat_rx(uint64_t msg_id,
                                              uint32_t from_id,
                                              uint32_t to_id,
                                              uint32_t sender_seq,
                                              uint32_t timestamp_s,
                                              uint16_t flags,
                                              const uint8_t *body,
                                              uint8_t body_len)
{
    /* Payload: [msg_id:8][from_id:4][to_id:4][sender_seq:4][ts:4][flags:2][blen:1][body:N]
     * Fixed header = 27 bytes + body. */
    uint8_t payload[27u + PRIVATE_CHAT_MAX_BODY];
    uint8_t blen = body_len;

    if (!s_session_active) {
        return;
    }
    if (blen > PRIVATE_CHAT_MAX_BODY) {
        blen = PRIVATE_CHAT_MAX_BODY;
    }

    uint8_t *d = payload;
    /* msg_id LE8 */
    for (int i = 0; i < 8; i++) { d[i] = (uint8_t)((msg_id >> (i*8)) & 0xFFu); }
    d += 8;
    /* from_id LE4 */
    d[0]=(uint8_t)(from_id&0xFF); d[1]=(uint8_t)((from_id>>8)&0xFF);
    d[2]=(uint8_t)((from_id>>16)&0xFF); d[3]=(uint8_t)((from_id>>24)&0xFF);
    d += 4;
    /* to_id LE4 */
    d[0]=(uint8_t)(to_id&0xFF); d[1]=(uint8_t)((to_id>>8)&0xFF);
    d[2]=(uint8_t)((to_id>>16)&0xFF); d[3]=(uint8_t)((to_id>>24)&0xFF);
    d += 4;
    /* sender_seq LE4 */
    d[0]=(uint8_t)(sender_seq&0xFF); d[1]=(uint8_t)((sender_seq>>8)&0xFF);
    d[2]=(uint8_t)((sender_seq>>16)&0xFF); d[3]=(uint8_t)((sender_seq>>24)&0xFF);
    d += 4;
    /* timestamp_s LE4 */
    d[0]=(uint8_t)(timestamp_s&0xFF); d[1]=(uint8_t)((timestamp_s>>8)&0xFF);
    d[2]=(uint8_t)((timestamp_s>>16)&0xFF); d[3]=(uint8_t)((timestamp_s>>24)&0xFF);
    d += 4;
    /* flags LE2 */
    d[0]=(uint8_t)(flags&0xFF); d[1]=(uint8_t)((flags>>8)&0xFF);
    d += 2;
    /* body_len */
    d[0] = blen;
    d += 1;
    /* body */
    if (blen > 0u && body) {
        memcpy(d, body, blen);
    }

    uint8_t total = (uint8_t)(27u + blen);
    (void)cp_send_packet(RIVR_CP_PKT_PRIVATE_CHAT_RX, 0u, payload, total);
}

void rivr_ble_companion_push_pchat_state(uint64_t msg_id,
                                          uint32_t peer_id,
                                          uint8_t state)
{
    /* Payload: [msg_id:8 LE][peer_id:4 LE][state:1] = 13 bytes */
    uint8_t payload[13];

    if (!s_session_active) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        payload[i] = (uint8_t)((msg_id >> (i*8)) & 0xFFu);
    }
    payload[8]  = (uint8_t)(peer_id & 0xFFu);
    payload[9]  = (uint8_t)((peer_id >> 8) & 0xFFu);
    payload[10] = (uint8_t)((peer_id >> 16) & 0xFFu);
    payload[11] = (uint8_t)((peer_id >> 24) & 0xFFu);
    payload[12] = state;

    (void)cp_send_packet(RIVR_CP_PKT_PRIVATE_CHAT_STATE, 0u, payload, sizeof(payload));
}

void rivr_ble_companion_push_delivery_receipt(uint64_t orig_msg_id,
                                               uint32_t sender_id,
                                               uint32_t timestamp_s,
                                               uint8_t status)
{
    /* Payload: [orig_msg_id:8 LE][sender_id:4 LE][ts:4 LE][status:1] = 17 bytes */
    uint8_t payload[17];

    if (!s_session_active) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        payload[i] = (uint8_t)((orig_msg_id >> (i*8)) & 0xFFu);
    }
    payload[8]  = (uint8_t)(sender_id & 0xFFu);
    payload[9]  = (uint8_t)((sender_id >> 8) & 0xFFu);
    payload[10] = (uint8_t)((sender_id >> 16) & 0xFFu);
    payload[11] = (uint8_t)((sender_id >> 24) & 0xFFu);
    payload[12] = (uint8_t)(timestamp_s & 0xFFu);
    payload[13] = (uint8_t)((timestamp_s >> 8) & 0xFFu);
    payload[14] = (uint8_t)((timestamp_s >> 16) & 0xFFu);
    payload[15] = (uint8_t)((timestamp_s >> 24) & 0xFFu);
    payload[16] = status;

    (void)cp_send_packet(RIVR_CP_PKT_DELIVERY_RECEIPT, 0u, payload, sizeof(payload));
}

#endif

