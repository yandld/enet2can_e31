/*
 * can_udp_gateway.h - UDP data endpoint for the CAN gateway protocol
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef CAN_UDP_GATEWAY_H_
#define CAN_UDP_GATEWAY_H_

#include <stdbool.h>

bool can_udp_gateway_init(void);
void can_udp_gateway_poll(void);

#endif /* CAN_UDP_GATEWAY_H_ */
