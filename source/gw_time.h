/*
 * gw_time.h - gateway monotonic clock (ns) and microsecond deadlines
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * v1 time base: the Cortex-M7 DWT cycle counter at 160 MHz (6.25 ns/tick),
 * extended to 64 bits by wrap tracking from the 1 ms SysTick (wrap period
 * ~26.8 s, so a 1 ms cadence is safe by 4+ orders of magnitude). The design
 * target (STM @ 80 MHz aligned to the EMAC 1588 counter, doc 03 §3.6) is a
 * Phase-2 swap behind this same API; nothing else touches the hw counter.
 *
 * ts_base / TIME continuity: the protocol's u32 ts_base is the low 32 bits
 * of the FULL ns clock, so it must stay continuous (mod 2^32) across CYCCNT
 * wraps. Hence even the lo32 getter derives from the 64-bit extension; all
 * getters take a ~10-cycle IRQ-masked section and are safe in any context.
 */
#ifndef GW_TIME_H_
#define GW_TIME_H_

#include <stdint.h>

#include "fsl_device_registers.h"

void gw_time_init(void);
/* Wrap tracker; call from the 1 ms SysTick handler. */
void gw_time_track_wrap(void);

/* Raw cycles (160 MHz), wrap-safe for deltas < 26.8 s. For local latency
 * measurement and T_agg deadlines; NOT for protocol timestamps. */
static inline uint32_t gw_cycles(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t gw_cycles_per_us(void)
{
    return SystemCoreClock / 1000000U;
}

static inline uint32_t gw_cycles_to_us(uint32_t cycles)
{
    return cycles / gw_cycles_per_us();
}

/* 64-bit extended cycle counter. Safe in any context. */
uint64_t gw_cycles64(void);

/* Full gateway monotonic time in ns. Safe in any context. */
uint64_t gw_now_ns(void);

/* Low 32 bits of the monotonic ns clock = E2CF ts_base. Safe in any context. */
static inline uint32_t gw_now_ns_lo32(void)
{
    return (uint32_t)gw_now_ns();
}

#endif /* GW_TIME_H_ */
