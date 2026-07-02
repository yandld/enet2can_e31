/*
 * status_led.h - three discrete status LEDs for the E2CF gateway.
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef STATUS_LED_H_
#define STATUS_LED_H_

#include <stdint.h>

void status_led_init(void);
void status_led_poll(uint32_t now_ms);
void status_led_fault(void);

#endif /* STATUS_LED_H_ */
