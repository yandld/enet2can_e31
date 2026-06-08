/*
 * status_led.h - three discrete status LEDs for the Ethernet<->6xCAN-FD gateway.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Three independent, active-low LEDs give an at-a-glance view of the gateway the
 * way a router's front panel does (is the box alive / is the network up / are the
 * CAN buses healthy):
 *
 *   LED1 SYS (PTC16): firmware liveness.
 *       heartbeat ~1 Hz = running normally;  solid = halted on a fault.
 *   LED2 NET (PTB22): Ethernet side.
 *       off = no link;  slow blink = link up, waiting for IP;  solid = ready (has IP).
 *   LED3 CAN (PTC14): the six CAN buses.
 *       off = idle;  blink = frames being forwarded;  solid = a channel is in
 *       error-passive / bus-off (wiring / termination / bitrate trouble).
 */
#ifndef STATUS_LED_H_
#define STATUS_LED_H_

/* Configure the three LED pins as GPIO outputs, all off. Call once after the clocks
 * and pin mux are up (i.e. after BOARD_InitHardware). */
void status_led_init(void);

/* Refresh the LEDs from the current Ethernet/CAN state. Call every super-loop pass;
 * it self-throttles, so it adds negligible cost to the forwarding loop. */
void status_led_poll(void);

/* Force the "halted" indication (SYS solid, NET/CAN off). Call from a fault handler. */
void status_led_fault(void);

#endif /* STATUS_LED_H_ */
