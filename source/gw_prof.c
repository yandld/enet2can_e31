/*
 * gw_prof.c - in-firmware data-plane profiler (segment timing + gauges)
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See gw_prof.h for the rationale. Each metric keeps count / min / max / sum and
 * a 32-bucket power-of-two histogram (bucket b counts samples whose most
 * significant set bit is b; bucket 0 also catches the value 0). p50 / p99 are
 * estimated from the histogram, so they carry a +/- sqrt(2) bucket uncertainty -
 * coarse on purpose, enough to point at the dominant segment. min / max / mean
 * are exact. Duration metrics store raw DWT cycles and are printed in
 * microseconds; gauge metrics store plain counts and are printed as-is.
 */
#include "gw_prof.h"

#if E2CF_PROFILE

#include <stdbool.h>
#include <stdint.h>

#include "fsl_debug_console.h"
#include "fsl_device_registers.h"
#include "gw_time.h"

#define PROF_SUB_BITS 3U
#define PROF_SUB (1U << PROF_SUB_BITS) /* linear sub-buckets per octave */
#define PROF_HIST_BUCKETS 256U         /* widest u32 maps to index 238 */

/* One accumulator. All fields are plain integers updated under a short global
 * IRQ-masked section, so reads in gw_prof_dump() see a consistent snapshot. */
typedef struct
{
    uint32_t count;
    uint32_t min;
    uint32_t max;
    uint64_t sum;
    uint32_t hist[PROF_HIST_BUCKETS];
} prof_entry_t;

static prof_entry_t s_prof[PROF_N];

/* Display metadata, indexed by gw_prof_id_t. `is_us` selects cycle->us
 * conversion (duration metrics) versus raw printing (gauge metrics). Names are
 * fixed-width so the table lines up with a plain %s. */
typedef struct
{
    const char *name; /* padded to 12 chars for column alignment */
    bool is_us;
} prof_meta_t;

static const prof_meta_t s_meta[PROF_N] = {
    [PROF_ETH_FRAME]    = { "eth_frame   ", true },
    [PROF_CAN_ISR]      = { "can_isr     ", true },
    [PROF_TX_FIFOWAIT]  = { "tx_fifowait ", true },
    [PROF_TX_BUS]       = { "tx_bus      ", true },
    [PROF_TX_GAP]       = { "tx_gap      ", true },
    [PROF_FACE_DWELL]   = { "face_dwell  ", true },
    [PROF_TXFIFO_DEPTH] = { "txfifo_depth", false },
    [PROF_AGG_RECORDS]  = { "agg_records ", false },
    [PROF_ETH_INFLIGHT] = { "eth_inflight", false },
};

/*
 * Sub-octave histogram bucket (HdrHistogram-style): values below 2*PROF_SUB are
 * counted exactly, and above that each power-of-two octave is split into
 * PROF_SUB linear sub-buckets, giving ~1/PROF_SUB (~12%) relative resolution.
 * The widest u32 value maps to index 238, so PROF_HIST_BUCKETS=256 never
 * overflows. Far finer than plain octave bins, whose midpoint could exceed a
 * tightly clustered metric's true max.
 */
static inline uint32_t prof_bucket(uint32_t value)
{
    uint32_t exp, sub;

    if (value < (PROF_SUB << 1))
    {
        return value; /* linear region: exact */
    }
    exp = 31U - __CLZ(value);
    sub = (value >> (exp - PROF_SUB_BITS)) - PROF_SUB;
    return ((exp - PROF_SUB_BITS) << PROF_SUB_BITS) + sub + PROF_SUB;
}

/*
 * Representative value (sub-bucket midpoint) for a bucket index: the inverse of
 * prof_bucket(), used when reporting a percentile.
 */
static inline uint32_t prof_bucket_rep(uint32_t bucket)
{
    uint32_t blk, exp, sub, base;

    if (bucket < (PROF_SUB << 1))
    {
        return bucket; /* linear region: exact */
    }
    blk = bucket - PROF_SUB;
    exp = (blk >> PROF_SUB_BITS) + PROF_SUB_BITS;
    sub = blk & (PROF_SUB - 1U);
    base = (PROF_SUB + sub) << (exp - PROF_SUB_BITS);
    return base + (1U << (exp - PROF_SUB_BITS - 1U));
}

/*
 * Record one sample for metric `id`. `value` is a gw_cycles() delta (duration
 * metrics) or a raw count (gauge metrics). Short IRQ-masked critical section:
 * safe from CAN ISR / EMAC ISR / main loop.
 */
void gw_prof_add(gw_prof_id_t id, uint32_t value)
{
    prof_entry_t *e;
    uint32_t primask;

    if ((uint32_t)id >= (uint32_t)PROF_N)
    {
        return;
    }
    e = &s_prof[id];

    primask = __get_PRIMASK();
    __disable_irq();
    if ((e->count == 0U) || (value < e->min))
    {
        e->min = value;
    }
    if (value > e->max)
    {
        e->max = value;
    }
    e->sum += value;
    e->count++;
    e->hist[prof_bucket(value)]++;
    __set_PRIMASK(primask);
}

/*
 * Estimate the value at `permille` (e.g. 500 = p50, 990 = p99) by walking the
 * histogram to the target rank and returning that bucket's representative.
 * Caller passes a stable snapshot (dump masks interrupts around the read set).
 */
static uint32_t prof_pct(const prof_entry_t *e, uint32_t permille)
{
    uint32_t target = (uint32_t)(((uint64_t)e->count * permille) / 1000U);
    uint32_t acc = 0U;
    uint32_t b;

    if (target == 0U)
    {
        target = 1U;
    }
    for (b = 0U; b < PROF_HIST_BUCKETS; b++)
    {
        acc += e->hist[b];
        if (acc >= target)
        {
            uint32_t rep = prof_bucket_rep(b);

            /* Bound the bucket estimate by the exact min/max so a percentile can
             * never fall outside the observed range. */
            if (rep < e->min)
            {
                rep = e->min;
            }
            if (rep > e->max)
            {
                rep = e->max;
            }
            return rep;
        }
    }
    return e->max;
}

/*
 * Print the whole profile table to the UART. Duration metrics are converted to
 * microseconds (sub-microsecond samples read as 0); gauge metrics print raw.
 * All printed quantities are u32 cast to unsigned: the lite PRINTF ignores the
 * 'l' length modifier, so %u/%8u are used throughout. Main-loop context only.
 */
void gw_prof_dump(void)
{
    uint32_t per_us = gw_cycles_per_us();
    uint32_t i;

    PRINTF("\r\n--- gw_prof (data-plane segments) ---\r\n");
    PRINTF("duration (us)   count      min      p50      p99      max     mean\r\n");
    for (i = 0U; i < (uint32_t)PROF_N; i++)
    {
        prof_entry_t snap;
        uint32_t primask;
        uint32_t mn, p50, p99, mx, mean;

        if (!s_meta[i].is_us)
        {
            continue;
        }

        primask = __get_PRIMASK();
        __disable_irq();
        snap = s_prof[i];
        __set_PRIMASK(primask);

        if (snap.count == 0U)
        {
            PRINTF("%s        0        -        -        -        -        -\r\n",
                   s_meta[i].name);
            continue;
        }
        mn = snap.min / per_us;
        p50 = prof_pct(&snap, 500U) / per_us;
        p99 = prof_pct(&snap, 990U) / per_us;
        mx = snap.max / per_us;
        mean = (uint32_t)(snap.sum / snap.count) / per_us;
        PRINTF("%s %8u %8u %8u %8u %8u %8u\r\n", s_meta[i].name,
               (unsigned)snap.count, (unsigned)mn, (unsigned)p50,
               (unsigned)p99, (unsigned)mx, (unsigned)mean);
    }

    PRINTF("gauge (count)   count      min      p50      p99      max     mean\r\n");
    for (i = 0U; i < (uint32_t)PROF_N; i++)
    {
        prof_entry_t snap;
        uint32_t primask;
        uint32_t mean;

        if (s_meta[i].is_us)
        {
            continue;
        }

        primask = __get_PRIMASK();
        __disable_irq();
        snap = s_prof[i];
        __set_PRIMASK(primask);

        if (snap.count == 0U)
        {
            PRINTF("%s        0        -        -        -        -        -\r\n",
                   s_meta[i].name);
            continue;
        }
        mean = (uint32_t)(snap.sum / snap.count);
        PRINTF("%s %8u %8u %8u %8u %8u %8u\r\n", s_meta[i].name,
               (unsigned)snap.count, (unsigned)snap.min,
               (unsigned)prof_pct(&snap, 500U), (unsigned)prof_pct(&snap, 990U),
               (unsigned)snap.max, (unsigned)mean);
    }
    PRINTF("-------------------------------------\r\n");
}

/* Zero every accumulator so the next dump covers a fresh window. */
void gw_prof_clear(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    for (uint32_t i = 0U; i < (uint32_t)PROF_N; i++)
    {
        s_prof[i].count = 0U;
        s_prof[i].min = 0U;
        s_prof[i].max = 0U;
        s_prof[i].sum = 0U;
        for (uint32_t b = 0U; b < PROF_HIST_BUCKETS; b++)
        {
            s_prof[i].hist[b] = 0U;
        }
    }
    __set_PRIMASK(primask);
}

#endif /* E2CF_PROFILE */
