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
/* Sample the super-loop period (DWT); call once per main-loop iteration. */
void can_udp_gateway_mark_loop(void);

#endif /* CAN_UDP_GATEWAY_H_ */
