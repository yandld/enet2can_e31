/*
 * can_udp_gateway.h - UDP data endpoint for the CAN gateway protocol
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef CAN_UDP_GATEWAY_H_
#define CAN_UDP_GATEWAY_H_

#include <stdbool.h>
#include <stdint.h>

bool can_udp_gateway_init(void);
void can_udp_gateway_poll(void);
/* Sample the super-loop period (DWT); call once per main-loop iteration. */
void can_udp_gateway_mark_loop(void);
/* Record the cycle count at which this iteration is about to drain the Ethernet MAC RX
 * ring (i.e. just before ethernet_lwip_poll). The UDP RX callback fires synchronously
 * inside that poll, so this is the frame's MAC-RX-pull instant: using it as the latency
 * origin folds the lwIP RX stack (eth/IP/UDP parse) into the measured board latency. */
void can_udp_gateway_mark_eth_rx(uint32_t cycles);
/* Record the three super-loop leg durations (DWT cycles): ethernet_lwip_poll,
 * can_service_poll, can_udp_gateway_poll. Call once per iteration after the polls. */
void can_udp_gateway_mark_legs(uint32_t ethCycles, uint32_t canCycles, uint32_t gwCycles);

#endif /* CAN_UDP_GATEWAY_H_ */
