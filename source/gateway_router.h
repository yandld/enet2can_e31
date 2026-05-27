/*
 * gateway_router.h - CAN gateway routing and status boundary
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef GATEWAY_ROUTER_H_
#define GATEWAY_ROUTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "can_gateway_protocol.h"
#include "can_service.h"
#include "ethernet_lwip.h"

typedef struct
{
    uint32_t rx;
    uint32_t tx;
    uint32_t drop;
    uint32_t parseError;
    uint32_t disabledChannel;
    uint32_t queueFull;
} gateway_router_counter_t;

typedef struct
{
    uint32_t activeMask;
    ethernet_lwip_status_t ethernet;
    gateway_router_counter_t data;
    can_service_status_t can[CAN_GATEWAY_MAX_CHANNELS];
    can_service_config_t canConfig[CAN_GATEWAY_MAX_CHANNELS];
    uint32_t configStatus;
} gateway_router_snapshot_t;

bool gateway_router_init(uint32_t activeMask);
uint32_t gateway_router_from_udp(const can_gateway_frame_t *frame);
bool gateway_router_next_udp_frame(can_gateway_frame_t *frame);
gateway_router_snapshot_t gateway_router_get_snapshot(void);
can_service_config_t gateway_router_get_config(uint8_t channel);
uint32_t gateway_router_set_can0_config(const can_service_config_t *config);

#endif /* GATEWAY_ROUTER_H_ */
