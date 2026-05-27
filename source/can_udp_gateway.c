/*
 * can_udp_gateway.c - UDP data and status endpoints for the CAN gateway
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "can_udp_gateway.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "can_gateway_protocol.h"
#include "fsl_debug_console.h"
#include "gateway_router.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define STATUS_JSON_SIZE 3600U
#define CONFIG_JSON_SIZE 1800U
#define CONTROL_REQUEST_SIZE 512U

static struct udp_pcb *s_dataPcb;
static struct udp_pcb *s_statusPcb;
static ip_addr_t s_peerAddr;
static uint16_t s_peerPort;
static bool s_initialized;
static bool s_peerKnown;
static uint32_t s_udpRxPacketCount;
static uint32_t s_udpTxPacketCount;
static uint32_t s_udpDropCount;
static uint32_t s_udpParseErrorCount;
static uint32_t s_udpNoPeerCount;
static uint32_t s_statusRxCount;
static uint32_t s_statusTxCount;

static bool packet_to_frame(const struct pbuf *p, can_gateway_frame_t *frame)
{
    if ((p == NULL) || (frame == NULL) || (p->tot_len != sizeof(can_gateway_frame_t)))
    {
        return false;
    }

    (void)pbuf_copy_partial((struct pbuf *)p, frame, sizeof(*frame), 0);
    return true;
}

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
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

static const char *find_value(const char *request, const char *key)
{
    const char *pos = request;
    size_t keyLen = strlen(key);

    while ((pos = strstr(pos, key)) != NULL)
    {
        char before = (pos == request) ? ' ' : pos[-1];
        char after = pos[keyLen];
        bool beforeOk = (before == ' ') || (before == '{') || (before == ',') || (before == '\n') || (before == '\r');
        bool afterOk = (after == ' ') || (after == '=') || (after == ':') || (after == '"');

        if (beforeOk && afterOk)
        {
            break;
        }
        pos += keyLen;
    }

    if (pos == NULL)
    {
        return NULL;
    }

    pos += keyLen;
    while ((*pos == ' ') || (*pos == '=') || (*pos == ':') || (*pos == '"'))
    {
        pos++;
    }

    return pos;
}

static bool parse_u32_field(const char *request, const char *key, uint32_t *value)
{
    const char *pos = find_value(request, key);

    if ((pos == NULL) || (value == NULL))
    {
        return false;
    }

    *value = (uint32_t)strtoul(pos, NULL, 0);
    return true;
}

static bool parse_bool_field(const char *request, const char *key, bool *value)
{
    const char *pos = find_value(request, key);

    if ((pos == NULL) || (value == NULL))
    {
        return false;
    }

    if ((strncmp(pos, "true", 4U) == 0) || (strncmp(pos, "on", 2U) == 0) || (*pos == '1'))
    {
        *value = true;
        return true;
    }

    if ((strncmp(pos, "false", 5U) == 0) || (strncmp(pos, "off", 3U) == 0) || (*pos == '0'))
    {
        *value = false;
        return true;
    }

    return false;
}

static bool value_starts_with(const char *request, const char *key, const char *value)
{
    const char *pos = find_value(request, key);

    return (pos != NULL) && (strncmp(pos, value, strlen(value)) == 0);
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

static void format_peer(char *buffer, size_t size)
{
    char peerIp[48];

    if (!s_peerKnown)
    {
        (void)snprintf(buffer, size, "none");
        return;
    }

    (void)ipaddr_ntoa_r(&s_peerAddr, peerIp, (int)sizeof(peerIp));
    (void)snprintf(buffer, size, "%s:%u", peerIp, (unsigned)s_peerPort);
}

static void learn_peer(const ip_addr_t *addr, uint16_t port)
{
    ip_addr_copy(s_peerAddr, *addr);
    s_peerPort = port;
    s_peerKnown = true;
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
    char peerText[64];
    size_t used = 0U;

    format_ipv4(snapshot.ethernet.ipv4Addr, ipText, sizeof(ipText));
    format_peer(peerText, sizeof(peerText));

    append_json(buffer,
                size,
                &used,
                "{\"version\":%u,\"link\":%s,\"dhcp\":%s,\"ip\":\"%s\",\"active_mask\":%u,\"peer\":\"%s\","
                "\"config_status\":%u,\"config_status_text\":\"%s\",",
                (unsigned)CAN_GATEWAY_VERSION,
                json_bool(snapshot.ethernet.linkUp),
                json_bool(snapshot.ethernet.dhcpBound),
                ipText,
                (unsigned)snapshot.activeMask,
                peerText,
                (unsigned)snapshot.configStatus,
                config_status_to_json(snapshot.configStatus));
    append_json(buffer,
                size,
                &used,
                "\"udp\":{\"rx\":%u,\"tx\":%u,\"drop\":%u,\"parse_error\":%u,\"no_peer\":%u,"
                "\"status_rx\":%u,\"status_tx\":%u},",
                (unsigned)s_udpRxPacketCount,
                (unsigned)s_udpTxPacketCount,
                (unsigned)s_udpDropCount,
                (unsigned)s_udpParseErrorCount,
                (unsigned)s_udpNoPeerCount,
                (unsigned)s_statusRxCount,
                (unsigned)s_statusTxCount);
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

static uint32_t send_frame_to_peer(const can_gateway_frame_t *frame)
{
    struct pbuf *p;
    err_t err;

    if ((s_dataPcb == NULL) || (frame == NULL))
    {
        s_udpDropCount++;
        return CAN_GATEWAY_STATUS_INVALID_PACKET;
    }

    if (!s_peerKnown)
    {
        s_udpNoPeerCount++;
        s_udpDropCount++;
        return CAN_GATEWAY_STATUS_NO_PEER;
    }

    p = pbuf_alloc(PBUF_TRANSPORT, sizeof(*frame), PBUF_RAM);
    if (p == NULL)
    {
        s_udpDropCount++;
        return CAN_GATEWAY_STATUS_QUEUE_FULL;
    }

    err = pbuf_take(p, frame, sizeof(*frame));
    if (err == ERR_OK)
    {
        err = udp_sendto(s_dataPcb, p, &s_peerAddr, s_peerPort);
    }

    pbuf_free(p);

    if (err == ERR_OK)
    {
        s_udpTxPacketCount++;
        return CAN_GATEWAY_STATUS_OK;
    }

    s_udpDropCount++;
    return CAN_GATEWAY_STATUS_CAN_TX_ERROR;
}

static void send_status_to_peer(uint8_t channel, uint32_t status)
{
    can_gateway_frame_t response;

    memset(&response, 0, sizeof(response));
    response.magic = CAN_GATEWAY_MAGIC;
    response.version = CAN_GATEWAY_VERSION;
    response.channel = (channel < CAN_GATEWAY_MAX_CHANNELS) ? channel : 0U;
    response.status = status;

    (void)send_frame_to_peer(&response);
}

static void send_json_to_status_peer(const ip_addr_t *addr, uint16_t port, const char *json)
{
    struct pbuf *p;
    size_t length;

    if ((s_statusPcb == NULL) || (addr == NULL) || (json == NULL))
    {
        return;
    }

    length = strlen(json);

    p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)length, PBUF_RAM);
    if (p == NULL)
    {
        s_udpDropCount++;
        return;
    }

    if ((pbuf_take(p, json, (u16_t)length) == ERR_OK) && (udp_sendto(s_statusPcb, p, addr, port) == ERR_OK))
    {
        s_statusTxCount++;
    }
    else
    {
        s_udpDropCount++;
    }

    pbuf_free(p);
}

static void send_status_json_to_peer(const ip_addr_t *addr, uint16_t port)
{
    char json[STATUS_JSON_SIZE];

    memset(json, 0, sizeof(json));
    build_status_json(json, sizeof(json));
    send_json_to_status_peer(addr, port, json);
}

static void send_config_json_to_peer(const ip_addr_t *addr, uint16_t port)
{
    char json[CONFIG_JSON_SIZE];

    memset(json, 0, sizeof(json));
    build_config_json(json, sizeof(json));
    send_json_to_status_peer(addr, port, json);
}

static void can_udp_gateway_data_recv(void *arg,
                                      struct udp_pcb *pcb,
                                      struct pbuf *p,
                                      const ip_addr_t *addr,
                                      uint16_t port)
{
    can_gateway_frame_t frame;
    uint32_t status;

    (void)arg;
    (void)pcb;

    if ((p == NULL) || (addr == NULL))
    {
        return;
    }

    if (p->tot_len == 1U)
    {
        learn_peer(addr, port);
        pbuf_free(p);
        return;
    }

    if (!packet_to_frame(p, &frame))
    {
        s_udpParseErrorCount++;
        s_udpDropCount++;
        send_status_to_peer(0U, CAN_GATEWAY_STATUS_PARSE_ERROR);
        pbuf_free(p);
        return;
    }

    pbuf_free(p);
    s_udpRxPacketCount++;

    status = gateway_router_from_udp(&frame);
    if (status != CAN_GATEWAY_STATUS_OK)
    {
        send_status_to_peer(frame.channel, status);
    }
}

static void parse_can0_config_request(const char *request, can_service_config_t *config)
{
    uint32_t value;
    bool boolValue;

    *config = gateway_router_get_config(0U);

    if (parse_bool_field(request, "enabled", &boolValue))
    {
        config->enabled = boolValue;
    }
    if (parse_bool_field(request, "fd", &boolValue))
    {
        config->useFD = boolValue;
    }
    if (parse_u32_field(request, "bitrate", &value))
    {
        config->bitRate = value;
    }
    if (parse_u32_field(request, "data_bitrate", &value))
    {
        config->bitRateFD = value;
    }
    if (parse_bool_field(request, "brs", &boolValue))
    {
        config->brs = boolValue;
    }
    if (value_starts_with(request, "filter", "id_mask"))
    {
        config->filterMode = CAN_SERVICE_FILTER_ID_MASK;
    }
    else if (value_starts_with(request, "filter", "accept_all"))
    {
        config->filterMode = CAN_SERVICE_FILTER_ACCEPT_ALL;
    }
    if (parse_u32_field(request, "filter_id", &value))
    {
        config->filterId = value;
    }
    if (parse_u32_field(request, "filter_mask", &value))
    {
        config->filterMask = value;
    }
    if (value_starts_with(request, "tx_drop_policy", "drop_oldest"))
    {
        config->txDropPolicy = CAN_SERVICE_TX_DROP_OLDEST;
    }
    else if (value_starts_with(request, "tx_drop_policy", "drop_newest"))
    {
        config->txDropPolicy = CAN_SERVICE_TX_DROP_NEWEST;
    }
}

static void handle_status_control_request(const char *request, const ip_addr_t *addr, uint16_t port)
{
    can_service_config_t config;

    if (strstr(request, "set_can0_config") != NULL)
    {
        parse_can0_config_request(request, &config);
        (void)gateway_router_set_can0_config(&config);
        send_config_json_to_peer(addr, port);
    }
    else if (strstr(request, "get_config") != NULL)
    {
        send_config_json_to_peer(addr, port);
    }
    else
    {
        /* "status" and "get_status" both map to the legacy status response. */
        send_status_json_to_peer(addr, port);
    }
}

static void can_udp_gateway_status_recv(void *arg,
                                        struct udp_pcb *pcb,
                                        struct pbuf *p,
                                        const ip_addr_t *addr,
                                        uint16_t port)
{
    char request[CONTROL_REQUEST_SIZE];
    ip_addr_t sourceAddr;
    uint16_t sourcePort;
    u16_t copyLen;

    (void)arg;
    (void)pcb;

    if ((p == NULL) || (addr == NULL))
    {
        return;
    }

    s_statusRxCount++;
    ip_addr_copy(sourceAddr, *addr);
    sourcePort = port;
    memset(request, 0, sizeof(request));
    copyLen = (p->tot_len < (CONTROL_REQUEST_SIZE - 1U)) ? p->tot_len : (CONTROL_REQUEST_SIZE - 1U);
    (void)pbuf_copy_partial(p, request, copyLen, 0);
    pbuf_free(p);
    handle_status_control_request(request, &sourceAddr, sourcePort);
}

bool can_udp_gateway_init(void)
{
    err_t err;

    if (s_initialized)
    {
        return true;
    }

    s_dataPcb = udp_new();
    s_statusPcb = udp_new();
    if ((s_dataPcb == NULL) || (s_statusPcb == NULL))
    {
        PRINTF("CAN UDP gateway: udp_new failed\r\n");
        return false;
    }

    err = udp_bind(s_dataPcb, IP_ADDR_ANY, CAN_GATEWAY_UDP_DATA_PORT);
    if (err != ERR_OK)
    {
        PRINTF("CAN UDP gateway: data bind failed err=%d\r\n", (int)err);
        udp_remove(s_dataPcb);
        udp_remove(s_statusPcb);
        s_dataPcb = NULL;
        s_statusPcb = NULL;
        return false;
    }

    err = udp_bind(s_statusPcb, IP_ADDR_ANY, CAN_GATEWAY_UDP_STATUS_PORT);
    if (err != ERR_OK)
    {
        PRINTF("CAN UDP gateway: status bind failed err=%d\r\n", (int)err);
        udp_remove(s_dataPcb);
        udp_remove(s_statusPcb);
        s_dataPcb = NULL;
        s_statusPcb = NULL;
        return false;
    }

    udp_recv(s_dataPcb, can_udp_gateway_data_recv, NULL);
    udp_recv(s_statusPcb, can_udp_gateway_status_recv, NULL);
    s_initialized = true;

    PRINTF("CAN UDP gateway: data_port=%u status_port=%u\r\n",
           CAN_GATEWAY_UDP_DATA_PORT,
           CAN_GATEWAY_UDP_STATUS_PORT);
    return true;
}

void can_udp_gateway_poll(void)
{
    can_gateway_frame_t frame;

    if (!s_initialized)
    {
        return;
    }

    while (gateway_router_next_udp_frame(&frame))
    {
        (void)send_frame_to_peer(&frame);
    }
}
