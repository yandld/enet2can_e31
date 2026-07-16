/*
 * dbg_log.c - globally switchable, ISR-safe deferred debug logging
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "dbg_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fsl_debug_console.h"
#include "fsl_device_registers.h"
#include "fsl_lpuart.h"   /* direct non-blocking LPUART TX (dbg_log_drain) */
#include "board.h"        /* BOARD_DEBUG_UART_BASEADDR */
#include "gw_prof.h"

volatile uint8_t g_dbg_level = E2CF_LOG_RUNTIME_LEVEL;
volatile uint32_t g_dbg_mask = E2CF_LOG_RUNTIME_MASK;

/* Byte ring; writers are any context (short IRQ-masked section reserves a
 * span), the single reader is the main loop. Size is a power of two. */
static uint8_t s_ring[E2CF_LOG_RING_SIZE];
static volatile uint32_t s_head; /* write index (monotonic, masked on use) */
static volatile uint32_t s_tail; /* read index, main loop only */
static volatile uint32_t s_dropped;

_Static_assert((E2CF_LOG_RING_SIZE & (E2CF_LOG_RING_SIZE - 1U)) == 0U,
               "E2CF_LOG_RING_SIZE must be a power of two");
#define RING_MASK (E2CF_LOG_RING_SIZE - 1U)

static const char s_level_tag[6] = { '-', 'E', 'W', 'I', 'D', 'T' };

void dbg_log_init(void)
{
    s_head = 0U;
    s_tail = 0U;
    s_dropped = 0U;
}

uint32_t dbg_log_dropped(void)
{
    return s_dropped;
}

void dbg_log_clear_dropped(void)
{
    s_dropped = 0U;
}

void dbg_log_emit(uint8_t level, uint32_t module, const char *fmt, ...)
{
    char line[E2CF_LOG_LINE_MAX];
    va_list ap;
    int n;
    uint32_t len;
    uint32_t primask;
    uint32_t head;

    (void)module;

    /* Prefix: severity tag + millisecond-ish timestamp from DWT cycles. */
    n = snprintf(line, sizeof(line), "[%c %10lu] ",
                 s_level_tag[(level <= E2CF_LOG_LEVEL_TRACE) ? level : 0U],
                 (unsigned long)(DWT->CYCCNT / (SystemCoreClock / 1000U)));
    if (n < 0)
    {
        return;
    }

    va_start(ap, fmt);
    n += vsnprintf(&line[n], sizeof(line) - (size_t)n - 2U, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    if (n > (int)(sizeof(line) - 3U))
    {
        n = (int)(sizeof(line) - 3U);
    }
    line[n++] = '\r';
    line[n++] = '\n';
    len = (uint32_t)n;

    /* Reserve a span in the ring under masked IRQs (copy outside would risk
     * interleaving with a same-or-lower-prio writer; the copy is <=160B,
     * ~100ns-class at 160 MHz, acceptable even at CAN ISR priority). */
    primask = __get_PRIMASK();
    __disable_irq();
    head = s_head;
    if ((head - s_tail + len) > E2CF_LOG_RING_SIZE)
    {
        s_dropped++;
        __set_PRIMASK(primask);
        return;
    }
    for (uint32_t i = 0U; i < len; i++)
    {
        s_ring[(head + i) & RING_MASK] = (uint8_t)line[i];
    }
    s_head = head + len;
    __set_PRIMASK(primask);
}

/*
 * Non-blocking debug-UART TX: write one byte iff the LPUART TX data register
 * is free, else return false. Mirrors uart_try_getchar below. Keeps
 * dbg_log_drain from ever busy-waiting on the UART, so a log burst can no
 * longer stall the superloop and delay the T_agg aggregation flush.
 */
static bool uart_try_putchar(uint8_t c)
{
    LPUART_Type *base = (LPUART_Type *)BOARD_DEBUG_UART_BASEADDR;

    if ((base->STAT & LPUART_STAT_TDRE_MASK) == 0U)
    {
        return false; /* TX busy: retry on the next drain call */
    }
    base->DATA = c;
    return true;
}

void dbg_log_drain(uint32_t budget)
{
    uint32_t tail = s_tail;
    uint32_t head = s_head; /* snapshot; writers only move it forward */

    /* Non-blocking: emit as many bytes as the LPUART will accept right now,
     * then return - never busy-wait. Previously a startup log burst blocked
     * ~ms here (blocking DbgConsole_Putchar), stalling the superloop and
     * delaying the T_agg flush, which spiked uplink latency. Backlog stays in
     * the ring; overflow is counted in s_dropped (readable via
     * dbg_log_dropped() and the stats dump). */
    while ((tail != head) && (budget != 0U))
    {
        if (!uart_try_putchar(s_ring[tail & RING_MASK]))
        {
            break;
        }
        tail++;
        budget--;
    }
    s_tail = tail;
}

/* ---- tiny runtime console --------------------------------------------- */

/* Non-blocking RX poll on the debug-console LPUART. The lite debug console
 * has no TryGetchar, so peek the LPUART STAT register directly.
 * (fsl_lpuart.h / board.h are included at the top of this file.) */
static bool uart_try_getchar(uint8_t *out)
{
    LPUART_Type *base = (LPUART_Type *)BOARD_DEBUG_UART_BASEADDR;

    if ((base->STAT & LPUART_STAT_RDRF_MASK) == 0U)
    {
        return false;
    }
    *out = (uint8_t)base->DATA;
    return true;
}

bool dbg_log_console_poll(void)
{
    static const uint32_t mask_cycle[] = {
        0xFFFFFFFFUL,
        DBG_M_CAN | DBG_M_PROTO | DBG_M_AGG,
        DBG_M_ETH | DBG_M_HB | DBG_M_CFG,
        DBG_M_STATS,
    };
    static uint8_t s_mask_idx;
    uint8_t c;

    if (!uart_try_getchar(&c))
    {
        return false;
    }

    if ((c >= (uint8_t)'0') && (c <= (uint8_t)'5'))
    {
        g_dbg_level = (uint8_t)(c - (uint8_t)'0');
        PRINTF("dbg: level=%u\r\n", (unsigned)g_dbg_level);
    }
    else if (c == (uint8_t)'m')
    {
        s_mask_idx = (uint8_t)((s_mask_idx + 1U) % (sizeof(mask_cycle) / sizeof(mask_cycle[0])));
        g_dbg_mask = mask_cycle[s_mask_idx];
        PRINTF("dbg: mask=0x%08lx\r\n", (unsigned long)g_dbg_mask);
    }
    else if (c == (uint8_t)'p')
    {
        /* Dump the data-plane profile, then clear it so the next press brackets
         * a fresh window. No-op when E2CF_PROFILE is 0. */
        gw_prof_dump();
        gw_prof_clear();
    }
    else if (c == (uint8_t)'?')
    {
        PRINTF("dbg keys: 0-5=level(NONE..TRACE) m=cycle mask s=stats p=profile ?=help\r\n");
    }
    /* 's' is handled by the caller (stats dump owns module data). */
    return (c == (uint8_t)'s');
}
