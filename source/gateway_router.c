/*
 * gateway_router.c - CAN gateway routing and status boundary
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "gateway_router.h"

#include <string.h>

static bool s_initialized;
static uint32_t s_activeMask;
static uint8_t s_nextTxChannel;
static gateway_router_counter_t s_dataCounter;

static bool gateway_router_channel_enabled(uint8_t channel)
{
    return (channel < CAN_GATEWAY_MAX_CHANNELS) && ((s_activeMask & (1UL << channel)) != 0U);
}

static bool gateway_router_validate_frame(const can_gateway_frame_t *frame)
{
    uint8_t knownFlags = CAN_GATEWAY_FLAG_FD | CAN_GATEWAY_FLAG_BRS | CAN_GATEWAY_FLAG_EXTENDED_ID |
                         CAN_GATEWAY_FLAG_REMOTE | CAN_GATEWAY_FLAG_ERROR;
    bool isExtended;
    bool isFD;

    if ((frame == NULL) || (frame->channel >= CAN_GATEWAY_MAX_CHANNELS) || (frame->dlc > CAN_FD_DLC) ||
        ((frame->flags & (uint8_t)~knownFlags) != 0U) || ((frame->flags & CAN_GATEWAY_FLAG_ERROR) != 0U))
    {
        return false;
    }

    isExtended = ((frame->flags & CAN_GATEWAY_FLAG_EXTENDED_ID) != 0U);
    isFD = ((frame->flags & CAN_GATEWAY_FLAG_FD) != 0U);

    if ((!isExtended && (frame->can_id > 0x7FFU)) || (isExtended && (frame->can_id > 0x1FFFFFFFUL)))
    {
        return false;
    }

    if (!isFD && (frame->dlc > CAN_CLASSIC_DLC))
    {
        return false;
    }

    if (isFD && ((frame->flags & CAN_GATEWAY_FLAG_REMOTE) != 0U))
    {
        return false;
    }

    return true;
}

bool gateway_router_init(uint32_t activeMask)
{
    if (s_initialized)
    {
        return true;
    }

    memset(&s_dataCounter, 0, sizeof(s_dataCounter));
    s_activeMask = activeMask & ((1UL << CAN_GATEWAY_MAX_CHANNELS) - 1UL);
    s_nextTxChannel = 0U;
    s_initialized = true;

    return true;
}

uint32_t gateway_router_from_udp(const can_gateway_frame_t *frame)
{
    uint32_t status;

    if (!gateway_router_validate_frame(frame))
    {
        s_dataCounter.parseError++;
        s_dataCounter.drop++;
        return CAN_GATEWAY_STATUS_PARSE_ERROR;
    }

    s_dataCounter.rx++;

    if (!gateway_router_channel_enabled(frame->channel))
    {
        s_dataCounter.disabledChannel++;
        s_dataCounter.drop++;
        return CAN_GATEWAY_STATUS_DISABLED_CHANNEL;
    }

    status = can_service_send(frame->channel, frame);
    if (status != CAN_GATEWAY_STATUS_OK)
    {
        s_dataCounter.drop++;
        if (status == CAN_GATEWAY_STATUS_QUEUE_FULL)
        {
            s_dataCounter.queueFull++;
        }
    }

    return status;
}

bool gateway_router_next_udp_frame(can_gateway_frame_t *frame)
{
    if (frame == NULL)
    {
        return false;
    }

    for (uint8_t offset = 0U; offset < CAN_GATEWAY_MAX_CHANNELS; offset++)
    {
        uint8_t channel = (uint8_t)((s_nextTxChannel + offset) % CAN_GATEWAY_MAX_CHANNELS);

        if (can_service_read(channel, frame))
        {
            s_nextTxChannel = (uint8_t)((channel + 1U) % CAN_GATEWAY_MAX_CHANNELS);
            s_dataCounter.tx++;
            return true;
        }
    }

    return false;
}

gateway_router_snapshot_t gateway_router_get_snapshot(void)
{
    gateway_router_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.activeMask = s_activeMask;
    snapshot.ethernet = ethernet_lwip_get_status();
    snapshot.data = s_dataCounter;

    for (uint8_t channel = 0U; channel < CAN_GATEWAY_MAX_CHANNELS; channel++)
    {
        snapshot.can[channel] = can_service_get_status(channel);
        snapshot.canConfig[channel] = can_service_get_config(channel);
    }
    snapshot.configStatus = can_service_get_last_config_status();

    return snapshot;
}

can_service_config_t gateway_router_get_config(uint8_t channel)
{
    return can_service_get_config(channel);
}

uint32_t gateway_router_set_config(uint8_t channel, const can_service_config_t *config)
{
    uint32_t status = can_service_set_config(channel, config);

    if ((status == CAN_SERVICE_CONFIG_OK) && (config != NULL))
    {
        if (config->enabled)
        {
            s_activeMask |= (1UL << channel);
        }
        else
        {
            s_activeMask &= ~(1UL << channel);
        }
    }

    return status;
}

void gateway_router_reset_stats(void)
{
    memset(&s_dataCounter, 0, sizeof(s_dataCounter));
    can_service_reset_stats();
}
