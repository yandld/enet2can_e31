/*
 * can_udp_gateway.c - UDP tunnel data and JSON control endpoints
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "can_udp_gateway.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "can_gateway_protocol.h"
#include "fsl_debug_console.h"
#include "gateway_router.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define STATUS_JSON_SIZE 4400U
#define CONFIG_JSON_SIZE 1800U
#define CAPABILITIES_JSON_SIZE 900U
#define ACK_JSON_SIZE 160U
#define CONTROL_REQUEST_SIZE 512U

typedef struct
{
    uint32_t rxPackets;
    uint32_t txPackets;
    uint32_t rxFrames;
    uint32_t txFrames;
    uint32_t drop;
    uint32_t parseError;
    uint32_t noSession;
    uint32_t loss;
    uint32_t rxSequence;
    uint32_t txSequence;
} tunnel_counter_t;

static struct udp_pcb *s_dataPcb;
static struct udp_pcb *s_controlPcb;
static ip_addr_t s_sessionAddr;
static uint16_t s_sessionPort;
static bool s_initialized;
static bool s_sessionKnown;
static bool s_rxSequenceValid;
static uint32_t s_expectedRxSequence;
static uint32_t s_controlRxCount;
static uint32_t s_controlTxCount;
static tunnel_counter_t s_tunnel;

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

/* DLC code -> payload byte count (CAN FD table). */
static uint8_t dlc_to_len(uint8_t dlc)
{
    static const uint8_t table[16] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
                                      8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U};
    return (dlc < 16U) ? table[dlc] : CAN_GATEWAY_MAX_DATA_LEN;
}

/* Payload bytes carried on the wire for a frame: FD uses the DLC table, Classic caps at 8. */
static uint8_t wire_data_len(uint8_t flags, uint8_t dlc)
{
    if ((flags & CAN_GATEWAY_FLAG_FD) != 0U)
    {
        return dlc_to_len(dlc);
    }
    return (dlc > 8U) ? 8U : dlc;
}

static const char *can_state_to_json(can_service_state_t state)
{
    switch (state)
    {
        case CAN_SERVICE_STATE_WARNING:
            return "warning";

        case CAN_SERVICE_STATE_ERROR_PASSIVE:
            return "error-passive";

        case CAN_SERVICE_STATE_BUS_OFF:
            return "bus-off";

        case CAN_SERVICE_STATE_ERROR_ACTIVE:
        default:
            return "error-active";
    }
}

static const char *filter_mode_to_json(can_service_filter_mode_t mode)
{
    return (mode == CAN_SERVICE_FILTER_ID_MASK) ? "id_mask" : "accept_all";
}

static const char *tx_drop_policy_to_json(can_service_tx_drop_policy_t policy)
{
    return (policy == CAN_SERVICE_TX_DROP_OLDEST) ? "drop_oldest" : "drop_newest";
}

static const char *config_status_to_json(uint32_t status)
{
    switch (status)
    {
        case CAN_SERVICE_CONFIG_OK:
            return "ok";

        case CAN_SERVICE_CONFIG_UNSUPPORTED_CHANNEL:
            return "unsupported-channel";

        case CAN_SERVICE_CONFIG_NULL:
            return "null-config";

        case CAN_SERVICE_CONFIG_INVALID_BITRATE:
            return "invalid-bitrate";

        case CAN_SERVICE_CONFIG_INVALID_MODE:
            return "invalid-mode";

        case CAN_SERVICE_CONFIG_INVALID_FILTER:
            return "invalid-filter";

        default:
            return "unknown";
    }
}

static void append_json(char *buffer, size_t size, size_t *used, const char *format, ...)
{
    va_list args;
    int written;
    size_t space;

    if ((buffer == NULL) || (used == NULL) || (*used >= size))
    {
        return;
    }

    space = size - *used;
    va_start(args, format);
    written = vsnprintf(&buffer[*used], space, format, args);
    va_end(args);

    if (written < 0)
    {
        return;
    }

    if ((size_t)written >= space)
    {
        *used = size - 1U;
    }
    else
    {
        *used += (size_t)written;
    }
}

static const char *json_locate_value(const char *request, const char *key)
{
    size_t keyLen = strlen(key);

    if ((request == NULL) || (key == NULL))
    {
        return NULL;
    }

    for (const char *pos = request; *pos != '\0'; pos++)
    {
        if ((*pos == '"') && (strncmp(pos + 1, key, keyLen) == 0) && (pos[1U + keyLen] == '"'))
        {
            pos += keyLen + 2U;
            while ((*pos == ' ') || (*pos == '\t') || (*pos == '\r') || (*pos == '\n'))
            {
                pos++;
            }
            if (*pos != ':')
            {
                return NULL;
            }
            pos++;
            while ((*pos == ' ') || (*pos == '\t') || (*pos == '\r') || (*pos == '\n'))
            {
                pos++;
            }
            return pos;
        }
    }

    return NULL;
}

static bool json_string_equals(const char *request, const char *key, const char *value)
{
    const char *pos = json_locate_value(request, key);
    size_t valueLen = strlen(value);

    return (pos != NULL) && (*pos == '"') && (strncmp(pos + 1, value, valueLen) == 0) &&
           (pos[1U + valueLen] == '"');
}

static bool json_get_u32(const char *request, const char *key, uint32_t *value)
{
    const char *pos = json_locate_value(request, key);
    uint32_t result = 0U;
    bool foundDigit = false;

    if ((pos == NULL) || (value == NULL))
    {
        return false;
    }

    while ((*pos >= '0') && (*pos <= '9'))
    {
        foundDigit = true;
        result = (result * 10U) + (uint32_t)(*pos - '0');
        pos++;
    }

    if (!foundDigit)
    {
        return false;
    }

    *value = result;
    return true;
}

static bool json_get_bool(const char *request, const char *key, bool *value)
{
    const char *pos = json_locate_value(request, key);

    if ((pos == NULL) || (value == NULL))
    {
        return false;
    }

    if (strncmp(pos, "true", 4U) == 0)
    {
        *value = true;
        return true;
    }
    if (strncmp(pos, "false", 5U) == 0)
    {
        *value = false;
        return true;
    }

    return false;
}

static void format_ipv4(uint32_t addr, char *buffer, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)&addr;

    (void)snprintf(buffer,
                   size,
                   "%u.%u.%u.%u",
                   (unsigned)bytes[0],
                   (unsigned)bytes[1],
                   (unsigned)bytes[2],
                   (unsigned)bytes[3]);
}

static void format_session(char *buffer, size_t size)
{
    char peerIp[48];

    if (!s_sessionKnown)
    {
        (void)snprintf(buffer, size, "none");
        return;
    }

    (void)ipaddr_ntoa_r(&s_sessionAddr, peerIp, (int)sizeof(peerIp));
    (void)snprintf(buffer, size, "%s:%u", peerIp, (unsigned)s_sessionPort);
}

static void update_data_session(const ip_addr_t *addr, uint16_t port)
{
    bool changed = !s_sessionKnown || !ip_addr_cmp(&s_sessionAddr, addr) || (s_sessionPort != port);
    char peerIp[48];

    if (changed)
    {
        (void)ipaddr_ntoa_r(addr, peerIp, (int)sizeof(peerIp));
        PRINTF("UDP data session peer=%s:%u\r\n", peerIp, (unsigned)port);
    }

    ip_addr_copy(s_sessionAddr, *addr);
    s_sessionPort = port;
    s_sessionKnown = true;
}

/*
 * Parse a v3 variable-length datagram into an in-memory packet. Each record is a
 * 16-byte head followed by wire_data_len(flags, dlc) payload bytes. Rejects any
 * truncated record or trailing bytes so a malformed datagram is dropped whole.
 */
static bool parse_packet(const struct pbuf *p, can_gateway_packet_t *packet)
{
    uint8_t buf[CAN_GATEWAY_MAX_PACKET_BYTES];
    uint16_t total;
    uint16_t offset;
    uint16_t count;

    if ((p == NULL) || (packet == NULL) ||
        (p->tot_len < CAN_GATEWAY_PACKET_HEADER_SIZE) || (p->tot_len > sizeof(buf)))
    {
        return false;
    }

    total = p->tot_len;
    (void)pbuf_copy_partial((struct pbuf *)p, buf, total, 0);

    memset(packet, 0, sizeof(*packet));
    memcpy(&packet->header, buf, CAN_GATEWAY_PACKET_HEADER_SIZE);

    if ((packet->header.magic != CAN_GATEWAY_MAGIC) || (packet->header.version != CAN_GATEWAY_VERSION) ||
        (packet->header.packet_type != CAN_GATEWAY_PACKET_TYPE_FRAMES) ||
        (packet->header.frame_count > CAN_GATEWAY_MAX_FRAMES_PER_PACKET))
    {
        return false;
    }

    count = packet->header.frame_count;
    offset = CAN_GATEWAY_PACKET_HEADER_SIZE;

    for (uint16_t i = 0U; i < count; i++)
    {
        can_gateway_frame_t *frame = &packet->frames[i];
        uint8_t len;

        if ((uint32_t)offset + CAN_GATEWAY_FRAME_HEAD_SIZE > total)
        {
            return false;
        }

        memcpy(frame, &buf[offset], CAN_GATEWAY_FRAME_HEAD_SIZE);
        len = wire_data_len(frame->flags, frame->dlc);

        if ((uint32_t)offset + CAN_GATEWAY_FRAME_HEAD_SIZE + len > total)
        {
            return false;
        }

        if (len != 0U)
        {
            memcpy(frame->data, &buf[offset + CAN_GATEWAY_FRAME_HEAD_SIZE], len);
        }
        offset += (uint16_t)(CAN_GATEWAY_FRAME_HEAD_SIZE + len);
    }

    return (offset == total);
}

static void log_control_result(const char *command, uint32_t status)
{
    PRINTF("UDP control cmd=%s status=%u\r\n", command, (unsigned)status);
}

static void track_rx_sequence(uint32_t sequence)
{
    if (!s_rxSequenceValid)
    {
        s_rxSequenceValid = true;
        s_expectedRxSequence = sequence + 1U;
        s_tunnel.rxSequence = sequence;
        return;
    }

    if (sequence > s_expectedRxSequence)
    {
        s_tunnel.loss += sequence - s_expectedRxSequence;
    }

    s_expectedRxSequence = sequence + 1U;
    s_tunnel.rxSequence = sequence;
}

static void append_config_json(char *buffer,
                               size_t size,
                               size_t *used,
                               const can_service_config_t *config,
                               uint8_t channel)
{
    append_json(buffer,
                size,
                used,
                "%s{\"ch\":%u,\"enabled\":%s,\"fd\":%s,\"bitrate\":%u,"
                "\"data_bitrate\":%u,\"brs\":%s,\"filter\":\"%s\",\"filter_id\":%u,"
                "\"filter_mask\":%u,\"tx_drop_policy\":\"%s\"}",
                (channel == 0U) ? "" : ",",
                (unsigned)channel,
                json_bool(config->enabled),
                json_bool(config->useFD),
                (unsigned)config->bitRate,
                (unsigned)config->bitRateFD,
                json_bool(config->brs),
                filter_mode_to_json(config->filterMode),
                (unsigned)config->filterId,
                (unsigned)config->filterMask,
                tx_drop_policy_to_json(config->txDropPolicy));
}

static void build_config_json(char *buffer, size_t size)
{
    gateway_router_snapshot_t snapshot = gateway_router_get_snapshot();
    size_t used = 0U;

    append_json(buffer,
                size,
                &used,
                "{\"version\":%u,\"config_status\":%u,\"config_status_text\":\"%s\",\"config\":[",
                (unsigned)CAN_GATEWAY_VERSION,
                (unsigned)snapshot.configStatus,
                config_status_to_json(snapshot.configStatus));

    for (uint8_t channel = 0U; channel < CAN_GATEWAY_MAX_CHANNELS; channel++)
    {
        append_config_json(buffer, size, &used, &snapshot.canConfig[channel], channel);
    }

    append_json(buffer, size, &used, "]}\n");
}

static void build_status_json(char *buffer, size_t size)
{
    gateway_router_snapshot_t snapshot = gateway_router_get_snapshot();
    char ipText[16];
    char sessionText[64];
    size_t used = 0U;

    format_ipv4(snapshot.ethernet.ipv4Addr, ipText, sizeof(ipText));
    format_session(sessionText, sizeof(sessionText));

    append_json(buffer,
                size,
                &used,
                "{\"version\":%u,\"link\":%s,\"dhcp\":%s,\"ip\":\"%s\",\"active_mask\":%u,"
                "\"config_status\":%u,\"config_status_text\":\"%s\",",
                (unsigned)CAN_GATEWAY_VERSION,
                json_bool(snapshot.ethernet.linkUp),
                json_bool(snapshot.ethernet.dhcpBound),
                ipText,
                (unsigned)snapshot.activeMask,
                (unsigned)snapshot.configStatus,
                config_status_to_json(snapshot.configStatus));
    append_json(buffer,
                size,
                &used,
                "\"tunnel\":{\"session\":\"%s\",\"rx_packets\":%u,\"tx_packets\":%u,"
                "\"rx_frames\":%u,\"tx_frames\":%u,\"drop\":%u,\"parse_error\":%u,"
                "\"no_session\":%u,\"loss\":%u,\"rx_seq\":%u,\"tx_seq\":%u,"
                "\"control_rx\":%u,\"control_tx\":%u},",
                sessionText,
                (unsigned)s_tunnel.rxPackets,
                (unsigned)s_tunnel.txPackets,
                (unsigned)s_tunnel.rxFrames,
                (unsigned)s_tunnel.txFrames,
                (unsigned)s_tunnel.drop,
                (unsigned)s_tunnel.parseError,
                (unsigned)s_tunnel.noSession,
                (unsigned)s_tunnel.loss,
                (unsigned)s_tunnel.rxSequence,
                (unsigned)s_tunnel.txSequence,
                (unsigned)s_controlRxCount,
                (unsigned)s_controlTxCount);
    append_json(buffer,
                size,
                &used,
                "\"router\":{\"rx\":%u,\"tx\":%u,\"drop\":%u,\"parse_error\":%u,"
                "\"disabled\":%u,\"queue_full\":%u},\"can\":[",
                (unsigned)snapshot.data.rx,
                (unsigned)snapshot.data.tx,
                (unsigned)snapshot.data.drop,
                (unsigned)snapshot.data.parseError,
                (unsigned)snapshot.data.disabledChannel,
                (unsigned)snapshot.data.queueFull);

    for (uint8_t channel = 0U; channel < CAN_GATEWAY_MAX_CHANNELS; channel++)
    {
        const can_service_status_t *can = &snapshot.can[channel];
        uint32_t errorCount = can->txErrorCount + can->rxErrorCount + can->txTimeoutCount;

        append_json(buffer,
                    size,
                    &used,
                    "%s{\"ch\":%u,\"enabled\":%s,\"fd\":%s,\"state\":\"%s\",\"rx\":%u,\"tx_start\":%u,"
                    "\"tx_done\":%u,\"tx_queue\":%u,\"rx_queue\":%u,\"tx_drop\":%u,"
                    "\"rx_drop\":%u,\"error\":%u,\"last_error\":%u,\"tx_err_counter\":%u,"
                    "\"rx_err_counter\":%u,\"rx_fifo_overflow\":%u,\"rx_fifo_warning\":%u,"
                    "\"rx_capacity\":%u,\"tx_capacity\":%u,\"rx_drain_max\":%u,"
                    "\"rx_hw_slots\":%u,\"tx_mb\":%u,\"rx_mb\":%u,\"rx_path\":\"%s\","
                    "\"watermark\":{\"rx\":%u,\"tx\":%u}}",
                    (channel == 0U) ? "" : ",",
                    (unsigned)channel,
                    json_bool(can->enabled),
                    json_bool(can->useFD),
                    can_state_to_json(can->state),
                    (unsigned)can->rxCount,
                    (unsigned)can->txStartCount,
                    (unsigned)can->txDoneCount,
                    (unsigned)can->txQueued,
                    (unsigned)can->rxQueued,
                    (unsigned)can->txDropCount,
                    (unsigned)can->rxDropCount,
                    (unsigned)errorCount,
                    (unsigned)can->lastErrorStatus,
                    (unsigned)can->txErrCounter,
                    (unsigned)can->rxErrCounter,
                    (unsigned)can->rxFifoOverflowCount,
                    (unsigned)can->rxFifoWarningCount,
                    (unsigned)can->rxCapacity,
                    (unsigned)can->txCapacity,
                    (unsigned)can->rxDrainMax,
                    (unsigned)can->rxHwSlots,
                    (unsigned)can->txMbCount,
                    (unsigned)can->rxMbCount,
                    can->enhancedRxFifo ? "efifo" : "mb-bank",
                    (unsigned)can->rxQueueWatermark,
                    (unsigned)can->txQueueWatermark);
    }

    append_json(buffer, size, &used, "],\"config\":[");
    for (uint8_t channel = 0U; channel < CAN_GATEWAY_MAX_CHANNELS; channel++)
    {
        append_config_json(buffer, size, &used, &snapshot.canConfig[channel], channel);
    }
    append_json(buffer, size, &used, "]}\n");
}

static void build_capabilities_json(char *buffer, size_t size)
{
    size_t used = 0U;

    append_json(buffer,
                size,
                &used,
                "{\"version\":%u,\"protocol\":\"socketcan-tunnel\",\"data_port\":%u,"
                "\"control_port\":%u,\"max_channels\":%u,\"max_frames_per_packet\":%u,"
                "\"max_data_len\":%u,\"rx_queue_capacity\":%u,\"tx_queue_capacity\":%u,"
                "\"rx_drain_max\":%u,\"commands\":[\"get_capabilities\",\"get_status\","
                "\"get_config\",\"set_can_config\",\"reset_stats\"]}\n",
                (unsigned)CAN_GATEWAY_VERSION,
                (unsigned)CAN_GATEWAY_UDP_DATA_PORT,
                (unsigned)CAN_GATEWAY_UDP_CONTROL_PORT,
                (unsigned)CAN_GATEWAY_MAX_CHANNELS,
                (unsigned)CAN_GATEWAY_MAX_FRAMES_PER_PACKET,
                (unsigned)CAN_GATEWAY_MAX_DATA_LEN,
                (unsigned)CAN_SERVICE_RX_RING_SIZE,
                (unsigned)CAN_SERVICE_TX_QUEUE_SIZE,
                (unsigned)CAN_SERVICE_RX_DRAIN_MAX);
}

static void build_ack_json(char *buffer, size_t size, const char *command)
{
    size_t used = 0U;

    append_json(buffer,
                size,
                &used,
                "{\"version\":%u,\"cmd\":\"%s\",\"status\":\"ok\"}\n",
                (unsigned)CAN_GATEWAY_VERSION,
                command);
}

/*
 * Drain queued CAN frames into one UDP datagram and send it. Lossless under
 * backpressure: the pbuf is allocated BEFORE any frame is consumed, so when the
 * lwIP heap is exhausted the frames stay queued in the CAN service and are
 * retried next poll (bounded latency, not loss). Frames are packed directly into
 * the pbuf payload in the v3 variable-length wire format. Returns the number of
 * frames flushed; 0 means nothing to send or heap backpressure.
 */
static uint16_t flush_rx_to_session(void)
{
    uint16_t pending = gateway_router_pending();
    uint16_t count;
    uint16_t allocLen;
    struct pbuf *p;
    uint8_t *buf;
    uint16_t off;
    uint16_t n = 0U;
    can_gateway_frame_t frame;
    can_gateway_packet_header_t header;
    err_t err;

    if (pending == 0U)
    {
        return 0U;
    }

    count = (pending > CAN_GATEWAY_MAX_FRAMES_PER_PACKET) ? CAN_GATEWAY_MAX_FRAMES_PER_PACKET : pending;
    allocLen = (uint16_t)(CAN_GATEWAY_PACKET_HEADER_SIZE + (count * CAN_GATEWAY_MAX_FRAME_RECORD));

    p = pbuf_alloc(PBUF_TRANSPORT, allocLen, PBUF_RAM);
    if (p == NULL)
    {
        return 0U; /* heap backpressure: leave frames queued, retry next poll */
    }

    buf = (uint8_t *)p->payload;
    off = CAN_GATEWAY_PACKET_HEADER_SIZE;

    while ((n < count) && gateway_router_peek_udp_frame(&frame))
    {
        uint8_t len = wire_data_len(frame.flags, frame.dlc);

        memcpy(&buf[off], &frame, CAN_GATEWAY_FRAME_HEAD_SIZE);
        if (len != 0U)
        {
            memcpy(&buf[off + CAN_GATEWAY_FRAME_HEAD_SIZE], frame.data, len);
        }
        off += (uint16_t)(CAN_GATEWAY_FRAME_HEAD_SIZE + len);
        n++;
    }

    if (n == 0U)
    {
        gateway_router_reset_peek();
        pbuf_free(p);
        return 0U;
    }

    header.magic = CAN_GATEWAY_MAGIC;
    header.version = CAN_GATEWAY_VERSION;
    header.packet_type = CAN_GATEWAY_PACKET_TYPE_FRAMES;
    header.frame_count = n;
    header.sequence = s_tunnel.txSequence;
    header.status = CAN_GATEWAY_STATUS_OK;
    memcpy(buf, &header, CAN_GATEWAY_PACKET_HEADER_SIZE);

    (void)pbuf_realloc(p, off);
    err = udp_sendto(s_dataPcb, p, &s_sessionAddr, s_sessionPort);
    pbuf_free(p);

    if (err == ERR_OK)
    {
        gateway_router_commit_peeked(); /* consume frames only now that the send succeeded */
        s_tunnel.txPackets++;
        s_tunnel.txFrames += n;
        s_tunnel.txSequence++;
        return n;
    }

    /* Send failed (e.g. no TX buffer): leave frames queued and retry next poll
     * instead of dropping them (bounded latency, not loss). */
    gateway_router_reset_peek();
    return 0U;
}

static void send_json_to_control_peer(const ip_addr_t *addr, uint16_t port, const char *json)
{
    struct pbuf *p;
    size_t length;

    if ((s_controlPcb == NULL) || (addr == NULL) || (json == NULL))
    {
        return;
    }

    length = strlen(json);
    p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)length, PBUF_RAM);
    if (p == NULL)
    {
        s_tunnel.drop++;
        PRINTF("UDP control tx drop: pbuf_alloc len=%u failed\r\n", (unsigned)length);
        return;
    }

    if ((pbuf_take(p, json, (u16_t)length) == ERR_OK) && (udp_sendto(s_controlPcb, p, addr, port) == ERR_OK))
    {
        s_controlTxCount++;
    }
    else
    {
        s_tunnel.drop++;
        PRINTF("UDP control tx drop: udp_send failed len=%u\r\n", (unsigned)length);
    }

    pbuf_free(p);
}

static void send_status_json_to_peer(const ip_addr_t *addr, uint16_t port)
{
    char json[STATUS_JSON_SIZE];

    memset(json, 0, sizeof(json));
    build_status_json(json, sizeof(json));
    send_json_to_control_peer(addr, port, json);
}

static void send_config_json_to_peer(const ip_addr_t *addr, uint16_t port)
{
    char json[CONFIG_JSON_SIZE];

    memset(json, 0, sizeof(json));
    build_config_json(json, sizeof(json));
    send_json_to_control_peer(addr, port, json);
}

static void send_capabilities_json_to_peer(const ip_addr_t *addr, uint16_t port)
{
    char json[CAPABILITIES_JSON_SIZE];

    memset(json, 0, sizeof(json));
    build_capabilities_json(json, sizeof(json));
    send_json_to_control_peer(addr, port, json);
}

static void send_ack_json_to_peer(const ip_addr_t *addr, uint16_t port, const char *command)
{
    char json[ACK_JSON_SIZE];

    memset(json, 0, sizeof(json));
    build_ack_json(json, sizeof(json), command);
    send_json_to_control_peer(addr, port, json);
}

static void can_udp_gateway_data_recv(void *arg,
                                      struct udp_pcb *pcb,
                                      struct pbuf *p,
                                      const ip_addr_t *addr,
                                      uint16_t port)
{
    can_gateway_packet_t packet;

    (void)arg;
    (void)pcb;

    if ((p == NULL) || (addr == NULL))
    {
        return;
    }

    /* Hot path: no blocking PRINTF here. Drops/parse-errors are counted and
     * surfaced via the JSON status endpoint instead. */
    if (!parse_packet(p, &packet))
    {
        s_tunnel.parseError++;
        s_tunnel.drop++;
        pbuf_free(p);
        return;
    }

    pbuf_free(p);
    update_data_session(addr, port);
    track_rx_sequence(packet.header.sequence);
    s_tunnel.rxPackets++;
    s_tunnel.rxFrames += packet.header.frame_count;

    for (uint16_t i = 0U; i < packet.header.frame_count; i++)
    {
        if (gateway_router_from_udp(&packet.frames[i]) != CAN_GATEWAY_STATUS_OK)
        {
            s_tunnel.drop++;
        }
    }
}

static uint32_t apply_config_request(const char *request)
{
    can_service_config_t config;
    uint32_t channel = 0U;
    uint32_t value;
    bool boolValue;

    (void)json_get_u32(request, "channel", &channel);
    if (channel >= CAN_GATEWAY_MAX_CHANNELS)
    {
        return CAN_SERVICE_CONFIG_UNSUPPORTED_CHANNEL;
    }

    config = gateway_router_get_config((uint8_t)channel);

    if (json_get_bool(request, "enabled", &boolValue))
    {
        config.enabled = boolValue;
    }
    if (json_get_bool(request, "fd", &boolValue))
    {
        config.useFD = boolValue;
    }
    if (json_get_u32(request, "bitrate", &value))
    {
        config.bitRate = value;
    }
    if (json_get_u32(request, "data_bitrate", &value))
    {
        config.bitRateFD = value;
    }
    if (json_get_bool(request, "brs", &boolValue))
    {
        config.brs = boolValue;
    }
    if (json_string_equals(request, "filter", "id_mask"))
    {
        config.filterMode = CAN_SERVICE_FILTER_ID_MASK;
    }
    else if (json_string_equals(request, "filter", "accept_all"))
    {
        config.filterMode = CAN_SERVICE_FILTER_ACCEPT_ALL;
    }
    if (json_get_u32(request, "filter_id", &value))
    {
        config.filterId = value;
    }
    if (json_get_u32(request, "filter_mask", &value))
    {
        config.filterMask = value;
    }
    if (json_string_equals(request, "tx_drop_policy", "drop_oldest"))
    {
        config.txDropPolicy = CAN_SERVICE_TX_DROP_OLDEST;
    }
    else if (json_string_equals(request, "tx_drop_policy", "drop_newest"))
    {
        config.txDropPolicy = CAN_SERVICE_TX_DROP_NEWEST;
    }

    return gateway_router_set_config((uint8_t)channel, &config);
}

static void reset_all_stats(void)
{
    memset(&s_tunnel, 0, sizeof(s_tunnel));
    s_rxSequenceValid = false;
    s_expectedRxSequence = 0U;
    s_controlRxCount = 0U;
    s_controlTxCount = 0U;
    gateway_router_reset_stats();
}

static void handle_control_request(const char *request, const ip_addr_t *addr, uint16_t port)
{
    if (json_string_equals(request, "cmd", "get_capabilities"))
    {
        log_control_result("get_capabilities", CAN_SERVICE_CONFIG_OK);
        send_capabilities_json_to_peer(addr, port);
    }
    else if (json_string_equals(request, "cmd", "get_config"))
    {
        log_control_result("get_config", CAN_SERVICE_CONFIG_OK);
        send_config_json_to_peer(addr, port);
    }
    else if (json_string_equals(request, "cmd", "set_can_config"))
    {
        uint32_t status = apply_config_request(request);
        log_control_result("set_can_config", status);
        send_config_json_to_peer(addr, port);
    }
    else if (json_string_equals(request, "cmd", "reset_stats"))
    {
        reset_all_stats();
        log_control_result("reset_stats", CAN_SERVICE_CONFIG_OK);
        send_ack_json_to_peer(addr, port, "reset_stats");
    }
    else
    {
        log_control_result("get_status", CAN_SERVICE_CONFIG_OK);
        send_status_json_to_peer(addr, port);
    }
}

static void can_udp_gateway_control_recv(void *arg,
                                         struct udp_pcb *pcb,
                                         struct pbuf *p,
                                         const ip_addr_t *addr,
                                         uint16_t port)
{
    char request[CONTROL_REQUEST_SIZE];
    ip_addr_t sourceAddr;
    char sourceIp[48];
    uint16_t sourcePort;
    u16_t copyLen;

    (void)arg;
    (void)pcb;

    if ((p == NULL) || (addr == NULL))
    {
        return;
    }

    s_controlRxCount++;
    ip_addr_copy(sourceAddr, *addr);
    sourcePort = port;
    memset(request, 0, sizeof(request));
    copyLen = (p->tot_len < (CONTROL_REQUEST_SIZE - 1U)) ? p->tot_len : (CONTROL_REQUEST_SIZE - 1U);
    (void)pbuf_copy_partial(p, request, copyLen, 0);
    pbuf_free(p);
    (void)ipaddr_ntoa_r(&sourceAddr, sourceIp, (int)sizeof(sourceIp));
    PRINTF("UDP control rx from %s:%u len=%u\r\n", sourceIp, (unsigned)sourcePort, (unsigned)copyLen);
    handle_control_request(request, &sourceAddr, sourcePort);
}

bool can_udp_gateway_init(void)
{
    err_t err;

    if (s_initialized)
    {
        return true;
    }

    s_dataPcb = udp_new();
    s_controlPcb = udp_new();
    if ((s_dataPcb == NULL) || (s_controlPcb == NULL))
    {
        PRINTF("CAN UDP gateway: udp_new failed\r\n");
        return false;
    }

    err = udp_bind(s_dataPcb, IP_ADDR_ANY, CAN_GATEWAY_UDP_DATA_PORT);
    if (err != ERR_OK)
    {
        PRINTF("CAN UDP gateway: data bind failed err=%d\r\n", (int)err);
        udp_remove(s_dataPcb);
        udp_remove(s_controlPcb);
        s_dataPcb = NULL;
        s_controlPcb = NULL;
        return false;
    }

    err = udp_bind(s_controlPcb, IP_ADDR_ANY, CAN_GATEWAY_UDP_CONTROL_PORT);
    if (err != ERR_OK)
    {
        PRINTF("CAN UDP gateway: control bind failed err=%d\r\n", (int)err);
        udp_remove(s_dataPcb);
        udp_remove(s_controlPcb);
        s_dataPcb = NULL;
        s_controlPcb = NULL;
        return false;
    }

    udp_recv(s_dataPcb, can_udp_gateway_data_recv, NULL);
    udp_recv(s_controlPcb, can_udp_gateway_control_recv, NULL);
    s_initialized = true;

    PRINTF("CAN UDP gateway: data_port=%u control_port=%u\r\n",
           CAN_GATEWAY_UDP_DATA_PORT,
           CAN_GATEWAY_UDP_CONTROL_PORT);
    return true;
}

void can_udp_gateway_poll(void)
{
    if (!s_initialized || !s_sessionKnown)
    {
        return;
    }

    /* Flush queued CAN frames in MTU-sized batches until drained or backpressured. */
    while (flush_rx_to_session() == CAN_GATEWAY_MAX_FRAMES_PER_PACKET)
    {
    }
}
