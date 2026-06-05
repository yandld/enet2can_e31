/*
 * latency_timer.h - DWT cycle-counter timing for sub-microsecond latency stats
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Cortex-M7 DWT cycle counter is a free-running 32-bit counter clocked at the
 * core frequency (160 MHz -> 6.25 ns/tick, wraps every ~26.8 s). It gives
 * ground-truth board-internal latency independent of any host clock; the 1 ms
 * SysTick is far too coarse for the sub-200 us forwarding path measured here.
 * Deltas are wrap-safe as long as the interval is < 26.8 s (always true here),
 * because uint32_t subtraction is modular.
 */
#ifndef LATENCY_TIMER_H_
#define LATENCY_TIMER_H_

#include <stdint.h>

#include "fsl_device_registers.h"

/* Enable the DWT cycle counter. Call once at startup after the clocks are up. */
static inline void latency_timer_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t latency_cycle_now(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t latency_cycles_to_us(uint32_t cycles)
{
    uint32_t perUs = (uint32_t)(SystemCoreClock / 1000000U);

    return cycles / ((perUs == 0U) ? 1U : perUs);
}

/* Running latency accumulator (count / max / sum), all kept in cycles. */
typedef struct
{
    uint32_t count;
    uint32_t maxCycles;
    uint64_t sumCycles;
} latency_stat_t;

static inline void latency_stat_reset(latency_stat_t *stat)
{
    stat->count = 0U;
    stat->maxCycles = 0U;
    stat->sumCycles = 0U;
}

static inline void latency_stat_add(latency_stat_t *stat, uint32_t cycles)
{
    stat->count++;
    stat->sumCycles += cycles;
    if (cycles > stat->maxCycles)
    {
        stat->maxCycles = cycles;
    }
}

static inline uint32_t latency_stat_avg_cycles(const latency_stat_t *stat)
{
    return (stat->count == 0U) ? 0U : (uint32_t)(stat->sumCycles / stat->count);
}

#endif /* LATENCY_TIMER_H_ */
