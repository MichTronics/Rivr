/**
 * @file  rivr_ble_companion.c
 * @brief Rivr BLE companion protocol layered on top of the NUS transport.
 */

#include "rivr_ble_companion.h"

/* Serial CP is available on any node role that owns UART0 (ESP32).
 * BLE-specific code is further guarded inside by #if RIVR_FEATURE_BLE. */
#if RIVR_FEATURE_BLE || RIVR_ROLE_CLIENT || RIVR_ROLE_REPEATER || RIVR_ROLE_GATEWAY

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>   /* write(), STDOUT_FILENO */

#include "driver/uart.h"
#include "../build_info.h"
#include "../private_chat.h"
#include "../radio_sx1262.h"
#include "../routing.h"
#include "../rivr_log.h"
#include "../rivr_metrics.h"
#include "../timebase.h"
#if RIVR_FEATURE_BLE
#include "rivr_ble.h"
#include "rivr_ble_service.h"
#endif
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
    RIVR_CP_CMD_SEND_CHAT         = 0x08u,  /**< Broadcast a PKT_CHAT frame */
    RIVR_CP_CMD_SEND_PRIVATE_V2  = 0x09u,  /**< Send private message with client_ref */
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
    RIVR_CP_PKT_METRICS_PUSH         = 0x8Bu,  /**< Full metrics snapshot — same payload as PKT_METRICS BLE frame */
};

typedef struct {
    uint8_t len;
    uint8_t origin;  /**< 0 = BLE, 1 = serial/UART0 */
    uint8_t data[RF_MAX_PAYLOAD_LEN];
} rivr_ble_companion_packet_t;

static rivr_ble_companion_packet_t s_rx_queue[RIVR_BLE_CP_QUEUE_LEN];
static volatile uint8_t s_rx_head = 0u;
static volatile uint8_t s_rx_tail = 0u;
/** Combined session flag — true while any transport session is active. */
static volatile bool s_session_active      = false;
/** Per-transport session flags for TX fan-out routing. */
static volatile bool s_ble_session_active    = false;
static volatile bool s_serial_session_active = false;

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
    bool sent = false;

    if (!s_ble_session_active && !s_serial_session_active) {
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

#if RIVR_FEATURE_BLE
    /* BLE transport */
    if (rivr_ble_is_connected() && s_ble_session_active) {
        sent |= rivr_ble_service_notify(rivr_ble_conn_handle(), packet, (uint8_t)total_len);
    }
#endif

    /* Serial/UART0 transport — SLIP-encode the packet and write to stdout.
     * SLIP (RFC 1055): each CP packet is framed between two 0xC0 (END) bytes.
     * 0xC0 in data → 0xDB 0xDC;  0xDB in data → 0xDB 0xDD. */
    if (s_serial_session_active) {
        /* Worst-case SLIP size: every byte is escaped (2×) + 2 END bytes */
        uint8_t slip_buf[RF_MAX_PAYLOAD_LEN * 2u + 2u];
        size_t slen = 0u;
        slip_buf[slen++] = 0xC0u;  /* SLIP_END — start-of-frame */
        for (size_t i = 0u; i < total_len; i++) {
            if (packet[i] == 0xC0u) {
                slip_buf[slen++] = 0xDBu;
                slip_buf[slen++] = 0xDCu;
            } else if (packet[i] == 0xDBu) {
                slip_buf[slen++] = 0xDBu;
                slip_buf[slen++] = 0xDDu;
            } else {
                slip_buf[slen++] = packet[i];
            }
        }
        slip_buf[slen++] = 0xC0u;  /* SLIP_END — end-of-frame */
        /* Use the same console abstraction as the RX side (rivr_cli.c) so
         * that SLIP frames reach the USB host on both UART and USB_SERIAL_JTAG
         * boards.  The UART VFS adds \n→\r\n translation that corrupts binary
         * frames, so bypass it with the raw driver.  On USB_SERIAL_JTAG boards
         * the UART0 peripheral is not connected to USB at all — write(STDOUT)
         * is required.                                                       */
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        (void)write(STDOUT_FILENO, slip_buf, slen);
#else
        uart_write_bytes(UART_NUM_0, (const char *)slip_buf, slen);
#endif
        sent = true;
    }

    return sent;
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

static void cp_handle_send_private_v2(const uint8_t *payload, uint8_t payload_len)
{
    /* Payload: [dst_id:4 LE][client_ref:4 LE][body:N] */
    if (!payload || payload_len < 8u) {
        cp_send_err(RIVR_CP_CMD_SEND_PRIVATE_V2, "payload too short");
        return;
    }

    uint32_t dst_id = (uint32_t)payload[0]
                    | ((uint32_t)payload[1] << 8)
                    | ((uint32_t)payload[2] << 16)
                    | ((uint32_t)payload[3] << 24);
    uint32_t client_ref = (uint32_t)payload[4]
                        | ((uint32_t)payload[5] << 8)
                        | ((uint32_t)payload[6] << 16)
                        | ((uint32_t)payload[7] << 24);

    const uint8_t *body     = &payload[8];
    uint8_t        body_len = (uint8_t)(payload_len - 8u);

    if (body_len > PRIVATE_CHAT_MAX_BODY) {
        uint8_t err_payload[5u + 32u];
        size_t msg_len = strlen("body too long");
        if (msg_len > 31u) msg_len = 31u;
        err_payload[0] = RIVR_CP_CMD_SEND_PRIVATE_V2;
        err_payload[1] = (uint8_t)(client_ref & 0xFFu);
        err_payload[2] = (uint8_t)((client_ref >> 8) & 0xFFu);
        err_payload[3] = (uint8_t)((client_ref >> 16) & 0xFFu);
        err_payload[4] = (uint8_t)((client_ref >> 24) & 0xFFu);
        memcpy(&err_payload[5], "body too long", msg_len);
        (void)cp_send_packet(RIVR_CP_PKT_ERR, 1u, err_payload, (uint8_t)(5u + msg_len));
        return;
    }

    uint64_t msg_id = 0u;
    pchat_error_t rc = private_chat_send(dst_id, body, body_len, &msg_id);
    if (rc != PCHAT_OK) {
        uint8_t err_payload[5u + 32u];
        size_t msg_len = strlen("send failed");
        if (msg_len > 31u) msg_len = 31u;
        err_payload[0] = RIVR_CP_CMD_SEND_PRIVATE_V2;
        err_payload[1] = (uint8_t)(client_ref & 0xFFu);
        err_payload[2] = (uint8_t)((client_ref >> 8) & 0xFFu);
        err_payload[3] = (uint8_t)((client_ref >> 16) & 0xFFu);
        err_payload[4] = (uint8_t)((client_ref >> 24) & 0xFFu);
        memcpy(&err_payload[5], "send failed", msg_len);
        (void)cp_send_packet(RIVR_CP_PKT_ERR, 1u, err_payload, (uint8_t)(5u + msg_len));
        return;
    }

    /* Respond with OK + echoed client_ref + real msg_id. */
    uint8_t ok_payload[13];
    ok_payload[0] = RIVR_CP_CMD_SEND_PRIVATE_V2;
    ok_payload[1] = (uint8_t)(client_ref & 0xFFu);
    ok_payload[2] = (uint8_t)((client_ref >> 8) & 0xFFu);
    ok_payload[3] = (uint8_t)((client_ref >> 16) & 0xFFu);
    ok_payload[4] = (uint8_t)((client_ref >> 24) & 0xFFu);
    for (uint8_t i = 0u; i < 8u; i++) {
        ok_payload[5u + i] = (uint8_t)((msg_id >> (i * 8u)) & 0xFFu);
    }
    (void)cp_send_packet(RIVR_CP_PKT_OK, 0u, ok_payload, sizeof(ok_payload));
}

#if RIVR_FEATURE_BLE
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
    s_rx_queue[s_rx_head].len    = (uint8_t)len;
    s_rx_queue[s_rx_head].origin = 0u;  /* BLE */
    s_rx_head = next_head;
    g_rivr_metrics.ble_rx_frames++;
    return true;
}
#endif /* RIVR_FEATURE_BLE */

bool rivr_serial_cp_handle_rx(const uint8_t *data, uint16_t len)
{
    uint8_t next_head;

    if (!data || len < RIVR_BLE_CP_HDR_LEN || len > RF_MAX_PAYLOAD_LEN) {
        return false;
    }
    if (data[0] != RIVR_BLE_CP_MAGIC0 || data[1] != RIVR_BLE_CP_MAGIC1) {
        return false;
    }
    if (data[2] != RIVR_BLE_CP_VERSION) {
        return true;
    }

    next_head = (uint8_t)((s_rx_head + 1u) % RIVR_BLE_CP_QUEUE_LEN);
    if (next_head == s_rx_tail) {
        RIVR_LOGW(TAG, "serial cp rx queue full");
        return true;
    }

    memcpy(s_rx_queue[s_rx_head].data, data, len);
    s_rx_queue[s_rx_head].len    = (uint8_t)len;
    s_rx_queue[s_rx_head].origin = 1u;  /* serial */
    s_rx_head = next_head;
    return true;
}

void rivr_serial_cp_session_stop(void)
{
    s_serial_session_active = false;
    s_session_active        = s_ble_session_active;
}

void rivr_serial_cp_start_session(void)
{
    s_serial_session_active = true;
    s_session_active        = true;
    cp_send_device_info();
}

bool rivr_serial_cp_session_active(void)
{
    return s_serial_session_active;
}

void rivr_serial_cp_push_metrics(const rivr_live_stats_t *live,
                                 uint32_t src_id, uint16_t net_id, uint16_t seq)
{
    if (!s_serial_session_active || !live) {
        return;
    }

    /* Build the exact same compact payload as rivr_metrics_ble_push() so
     * the app can reuse the existing RivrFrameCodec.parseFrame() path.     */
    rivr_met_ble_payload_t pl;
    memset(&pl, 0, sizeof(pl));
    pl.node_id       = live->node_id;
    pl.dc_pct        = live->dc_pct;
    pl.q_depth       = live->q_depth;
    pl.tx_total      = live->tx_total;
    pl.rx_total      = live->rx_total;
    pl.route_cache   = live->route_cache;
    pl.lnk_cnt       = live->lnk_cnt;
    pl.lnk_best      = live->lnk_best;
    pl.lnk_rssi      = live->lnk_best_rssi;
    pl.lnk_loss      = live->lnk_avg_loss;
    pl.relay_skip    = g_rivr_metrics.flood_fwd_cancelled_opport_total
                       + g_rivr_metrics.flood_fwd_score_suppressed_total;
    pl.relay_delay   = g_rivr_metrics.relay_delay_ms_total;
    pl.relay_density = live->relay_density;
    pl.relay_fwd     = g_rivr_metrics.relay_forwarded_total;
    pl.relay_sel     = g_rivr_metrics.flood_fwd_attempted_total;
    pl.relay_can     = g_rivr_metrics.flood_fwd_cancelled_opport_total;
    pl.rx_fail       = g_rivr_metrics.rx_decode_fail;
    pl.rx_dup        = g_rivr_metrics.rx_dedupe_drop;
    pl.rx_ttl        = g_rivr_metrics.rx_ttl_drop;
    pl.rx_bad_type   = g_rivr_metrics.rx_invalid_type;
    pl.rx_bad_hop    = g_rivr_metrics.rx_invalid_hop;
    pl.tx_full       = g_rivr_metrics.tx_queue_full;
    pl.dc_blk        = g_rivr_metrics.duty_blocked;
    pl.no_route      = g_rivr_metrics.drop_no_route;
    pl.loop_drop_total = g_rivr_metrics.loop_detect_drop_total;
    pl.rad_rst       = g_rivr_metrics.radio_hard_reset;
    pl.rad_txfail    = g_rivr_metrics.radio_tx_fail;
    pl.rad_crc       = g_rivr_metrics.radio_rx_crc_fail;
    pl.rc_hit        = g_rivr_metrics.route_cache_hit_total;
    pl.rc_miss       = g_rivr_metrics.route_cache_miss_total;
    pl.ack_tx        = g_rivr_metrics.ack_tx_total;
    pl.ack_rx        = g_rivr_metrics.ack_rx_total;
    pl.retry_att     = g_rivr_metrics.retry_attempt_total;
    pl.retry_ok      = g_rivr_metrics.retry_success_total;
    pl.retry_fail    = g_rivr_metrics.retry_fail_total;
    pl.ble_conn      = g_rivr_metrics.ble_connections;
    pl.ble_rx        = g_rivr_metrics.ble_rx_frames;
    pl.ble_tx        = g_rivr_metrics.ble_tx_frames;
    pl.ble_err       = g_rivr_metrics.ble_errors;

    /* Encode as a PKT_METRICS Rivr frame — same wire format as BLE path.   */
    rivr_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkt_type    = PKT_METRICS;
    hdr.ttl         = 1u;
    hdr.hop         = 0u;
    hdr.net_id      = net_id;
    hdr.src_id      = src_id;
    hdr.dst_id      = 0u;
    hdr.seq         = seq;
    hdr.pkt_id      = seq;
    hdr.payload_len = (uint8_t)sizeof(pl);

    uint8_t frame[23u + RIVR_MET_BLE_PAYLOAD_LEN + 2u];
    int len = protocol_encode(&hdr, (const uint8_t *)&pl,
                              (uint8_t)sizeof(pl),
                              frame, (uint8_t)sizeof(frame));
    if (len <= 0) {
        return;
    }

    (void)cp_send_packet(RIVR_CP_PKT_METRICS_PUSH, 0u, frame, (uint8_t)len);
}

void rivr_serial_cp_push_device_info(void)
{
    if (s_serial_session_active) {
        cp_send_device_info();
    }
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
            if (packet.origin == 1u) {
                s_serial_session_active = true;
            } else {
                s_ble_session_active = true;
            }
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

        case RIVR_CP_CMD_SEND_PRIVATE_V2:
            if (!s_session_active) {
                cp_send_err(cmd, "app start required");
                break;
            }
            cp_handle_send_private_v2(payload, payload_len);
            break;

        case RIVR_CP_CMD_SEND_CHAT:
            if (!s_session_active) {
                cp_send_err(cmd, "app start required");
                break;
            }
            if (payload_len == 0u || payload_len > RIVR_BLE_CP_MAX_CHAT_LEN) {
                cp_send_err(cmd, "invalid text length");
                break;
            }
            /* Push directly to rf_tx_queue — the same path as the ASCII
             * 'chat' CLI command.  Do NOT use rf_rx_ringbuf: the routing
             * layer filters frames whose src_id == g_my_node_id as loop
             * echoes and will silently discard them.                        */
            {
                rivr_pkt_hdr_t tx_hdr;
                memset(&tx_hdr, 0, sizeof(tx_hdr));
                tx_hdr.pkt_type    = PKT_CHAT;
                tx_hdr.ttl         = RIVR_PKT_DEFAULT_TTL;
                tx_hdr.net_id      = g_net_id;
                tx_hdr.src_id      = g_my_node_id;
                tx_hdr.dst_id      = 0u;
                tx_hdr.seq         = (uint16_t)++g_ctrl_seq;
                tx_hdr.pkt_id      = (uint16_t)g_ctrl_seq;
                tx_hdr.payload_len = payload_len;
                rf_tx_request_t req;
                memset(&req, 0, sizeof(req));
                int enc = protocol_encode(&tx_hdr, payload, payload_len,
                                         req.data, sizeof(req.data));
                if (enc > 0) {
                    req.len    = (uint8_t)enc;
                    req.toa_us = RF_TOA_APPROX_US(req.len);
                    req.due_ms = 0u;
                    (void)rb_try_push(&rf_tx_queue, &req);
                }
            }
            /* Echo back so the message appears immediately in the app. */
            rivr_ble_companion_push_chat(g_my_node_id, payload, payload_len);
            break;

        default:
            if (s_session_active) {
                cp_send_err(cmd, "unknown command");
            }
            break;
        }
    }
}

#if RIVR_FEATURE_BLE
void rivr_ble_companion_on_disconnect(void)
{
    s_ble_session_active = false;
    /* Keep serial session alive if one exists */
    s_session_active = s_serial_session_active;
    s_rx_head = 0u;
    s_rx_tail = 0u;
}

bool rivr_ble_companion_raw_bridge_enabled(void)
{
    return !s_session_active;
}
#endif /* RIVR_FEATURE_BLE */

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
    bool sent = cp_send_packet(RIVR_CP_PKT_PRIVATE_CHAT_RX, 0u, payload, total);
    RIVR_LOGI(TAG, "PM_RX_CP_SENT msg_id=0x%016" PRIx64 " total=%u sent=%d serial=%d",
              msg_id, (unsigned)total, (int)sent,
              (int)s_serial_session_active);
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
