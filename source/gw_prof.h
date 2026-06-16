/*
 * gw_prof.h - in-firmware data-plane profiler (segment timing + gauges)
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * canperf on the Linux side splits the app-to-app latency into five exact
 * segments, but its L3 (MCU residency, eth-in to eth-out) is a black box: it
 * lumps the downlink demux, the wait behind the single per-channel TX mailbox,
 * the physical CAN bus time, and the uplink aggregation dwell into one number.
 * This profiler breaks that box open on the MCU itself.
 *
 * Each metric accumulates count / min / max / sum plus a 32-bucket power-of-two
 * histogram, so a dump yields min / p50 / p99 / max / mean with no per-frame
 * protocol traffic (the wire format is untouched). Duration metrics are fed raw
 * DWT cycles (gw_cycles(), 160 MHz) and printed in microseconds; gauge metrics
 * are fed plain counts and printed as-is.
 *
 * Usage to bracket one run on the UART console: press 'p' (dumps then clears the
 * table), send the test traffic, press 'p' again to read that run's profile.
 *
 * Compile-time gated by E2CF_PROFILE: with it 0 every hook below collapses to a
 * no-op so the production data plane carries zero profiling overhead.
 *
 * All entry points take a short global-IRQ-masked critical section and are safe
 * from any context (CAN ISR prio 1, EMAC ISR prio 2, main loop).
 */
#ifndef GW_PROF_H_
#define GW_PROF_H_

#include <stdint.h>

#include "e2cf_config.h"

#ifndef E2CF_PROFILE
#define E2CF_PROFILE 0
#endif

/*
 * Metric identifiers. Duration metrics (fed gw_cycles() deltas, dumped in us)
 * come first, gauge metrics (fed raw counts) follow; gw_prof.c keeps a parallel
 * name/unit table indexed by this enum.
 */
typedef enum
{
    /* ---- duration metrics: gw_cycles() deltas, dumped in microseconds ---- */
    PROF_ETH_FRAME = 0, /* e2cf_core_eth_frame(): EMAC RX demux + submit cost  */
    PROF_CAN_ISR,       /* can_irq(): CAN RX+TX interrupt CPU cost             */
    PROF_TX_FIFOWAIT,   /* sw_txfifo enqueue -> TX MB load (wait behind 1 MB)  */
    PROF_TX_BUS,        /* TX MB load -> bus TX complete (physical CAN ~111us) */
    PROF_TX_GAP,        /* bus reload dead-time: TX complete -> next MB write  */
    PROF_FACE_DWELL,    /* DATA face open -> flush (uplink aggregation wait)   */

    /* ---- gauge metrics: raw counts, dumped as-is ---- */
    PROF_TXFIFO_DEPTH,  /* sw_txfifo occupancy sampled at enqueue (0..16)      */
    PROF_AGG_RECORDS,   /* records carried by a DATA face at flush (1..17)     */
    PROF_ETH_INFLIGHT,  /* eth_raw_tx_inflight() sampled at face send          */

    PROF_N
} gw_prof_id_t;

#if E2CF_PROFILE

/*
 * Record one sample for metric `id`. `value` is a gw_cycles() delta for
 * duration metrics or a raw count for gauge metrics.
 */
void gw_prof_add(gw_prof_id_t id, uint32_t value);

/* Print the full profile table (one row per metric) to the UART console. */
void gw_prof_dump(void);

/* Zero every metric so the next dump covers a fresh measurement window. */
void gw_prof_clear(void);

#else /* !E2CF_PROFILE: every hook compiles away */

static inline void gw_prof_add(gw_prof_id_t id, uint32_t value)
{
    (void)id;
    (void)value;
}
static inline void gw_prof_dump(void) {}
static inline void gw_prof_clear(void) {}

#endif /* E2CF_PROFILE */

#endif /* GW_PROF_H_ */
