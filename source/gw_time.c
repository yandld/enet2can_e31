/*
 * gw_time.c - gateway monotonic clock from the DWT cycle counter
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "gw_time.h"

static volatile uint32_t s_hi;      /* upper 32 bits of the cycle counter */
static volatile uint32_t s_prev_lo; /* CYCCNT at the last tracker run */

void gw_time_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_hi = 0U;
    s_prev_lo = 0U;
}

void gw_time_track_wrap(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t cur;

    __disable_irq();
    cur = DWT->CYCCNT;
    if (cur < s_prev_lo)
    {
        s_hi++;
    }
    s_prev_lo = cur;
    __set_PRIMASK(primask);
}

uint64_t gw_cycles64(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t hi, prev, lo;

    /* (hi, prev_lo) is only ever updated under masked IRQs (tracker above),
     * so masking here makes the triple read consistent in any context. */
    __disable_irq();
    hi = s_hi;
    prev = s_prev_lo;
    lo = DWT->CYCCNT;
    __set_PRIMASK(primask);

    if (lo < prev) /* wrapped since the last tracker run */
    {
        hi++;
    }
    return ((uint64_t)hi << 32) | lo;
}

uint64_t gw_now_ns(void)
{
    /* 160 MHz: ns = cycles * 6.25 = cycles * 25 / 4. */
    return (gw_cycles64() * 25U) >> 2;
}
