/*
 * e2cf_core.h - E2CF protocol engine (aggregation, window, CFG, HB)
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Glue between can_hw and eth_raw:
 *   CAN ISR (prio 1)  --can_rx/can_txc-->  DATA/TXC aggregation buffers
 *   EMAC ISR (prio 2) --eth_frame------->  demux: DATA->can_hw, CFG->exec+RSP,
 *                                          HB->link supervision
 *   main loop         --poll------------>  T_agg deadlines, HB/EVT/TIME ticks,
 *                                          link supervision, face reclaim
 */
#ifndef E2CF_CORE_H_
#define E2CF_CORE_H_

#include <stdbool.h>
#include <stdint.h>

bool e2cf_core_init(void);

/* Run all periodic work; call every superloop iteration. */
void e2cf_core_poll(void);

/* True once a valid peer HB has been received and not yet timed out. */
bool e2cf_core_link_ready(void);

void e2cf_core_dump_stats(void);

/* Upcall prototypes (implemented here, declared in can_hw.h / eth_raw.h):
 *   e2cf_core_can_rx / e2cf_core_can_txc / e2cf_core_can_state_change
 *   e2cf_core_eth_frame
 */

#endif /* E2CF_CORE_H_ */
