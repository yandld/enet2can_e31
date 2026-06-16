/*
 * eth_raw.h - bare ENET_QOS (EMAC) layer: raw L2 frames, no lwIP, no IP stack
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The E2CF gateway is a pure L2 endpoint (EtherType 0x88B5); the whole TCP/IP
 * middleware of the donor project is replaced by this thin layer:
 *   RX: EMAC DMA-ch0 interrupt (prio 2) -> zero-copy ENET_QOS_GetRxFrame ->
 *       e2cf_core_eth_frame() upcall, buffer returned to the pool immediately
 *       after parsing (records are copied out during the parse).
 *   TX: zero-copy ENET_QOS_SendFrame straight from the caller's buffer, which
 *       must sit in DMA-reachable, cache-coherent memory (the e2cf_core TX
 *       faces live in the DTCM non-cacheable section) and stay untouched
 *       until eth_raw_tx_inflight() shows it reclaimed.
 */
#ifndef ETH_RAW_H_
#define ETH_RAW_H_

#include <stdbool.h>
#include <stdint.h>

#include "fsl_common.h"

bool eth_raw_init(void);

/* PHY link supervision; call every ~100 ms from the main loop. */
void eth_raw_poll_link(void);
bool eth_raw_link_up(void);

/* Transmit one frame, zero-copy (see header note on buffer lifetime).
 * Safe from any context (short IRQ-masked section around the descriptor). */
status_t eth_raw_send(uint8_t *frame, uint32_t len);

/* Frames handed to DMA and not yet reclaimed. 0 = egress idle. */
uint32_t eth_raw_tx_inflight(void);

/* Monotonic 32-bit TX counters for per-frame completion tracking. Frames
 * complete in submission order (single FIFO ring): the Nth submitted frame
 * is done once eth_raw_tx_completed() reaches N. eth_raw_tx_submitted(),
 * read right after a successful eth_raw_send(), yields that frame's serial.
 * Single aligned-word reads, atomic on the Cortex-M7. */
uint32_t eth_raw_tx_submitted(void);
uint32_t eth_raw_tx_completed(void);

const uint8_t *eth_raw_mac(void);
void eth_raw_dump_stats(void);

/* Counter snapshot for the E2CF STATS push (plain u32 reads, same
 * per-counter-atomic guarantee as the UART dump). */
typedef struct
{
    uint32_t rx_frames;
    uint32_t rx_errors;
    uint32_t rx_drops;
    uint32_t tx_frames;
    uint32_t tx_busy;
} eth_raw_stats_t;

void eth_raw_get_stats(eth_raw_stats_t *out);
/* Zero the counters above. Does NOT touch the TX submitted/reclaimed
 * bookkeeping (eth_raw_tx_inflight depends on it) or link state. */
void eth_raw_clear_stats(void);

/* ---- upcall implemented by e2cf_core (EMAC IRQ context, prio 2) ---- */
void e2cf_core_eth_frame(const uint8_t *frame, uint32_t len);

#endif /* ETH_RAW_H_ */
