/*
 * dbg_log.h - globally switchable, ISR-safe deferred debug logging
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Design rules (doc 03 §2.2: no busy-wait in any ISR):
 *  - A log call NEVER touches the UART directly. It formats the line into a
 *    lock-free-enough ring buffer (short IRQ-masked critical section) and
 *    returns; the main superloop drains the ring to the debug UART.
 *  - Two-level gating:
 *      compile time : E2CF_LOG_BUILD_LEVEL strips higher levels entirely
 *      runtime      : g_dbg_level (severity) AND g_dbg_mask (module bits)
 *  - Per-module bit lets you watch one subsystem at TRACE while the rest
 *    stays quiet. Runtime control via dbg_log_set_*() or single-key UART
 *    console (see dbg_log_console_poll).
 *
 * Usage:  LOG_INF(DBG_M_CAN, "CAN%u started %u/%ukbps", ch, nom, dat);
 */
#ifndef DBG_LOG_H_
#define DBG_LOG_H_

#include <stdbool.h>
#include <stdint.h>

#include "e2cf_config.h"

/* Severity levels. */
#define E2CF_LOG_LEVEL_NONE  0
#define E2CF_LOG_LEVEL_ERROR 1
#define E2CF_LOG_LEVEL_WARN  2
#define E2CF_LOG_LEVEL_INFO  3
#define E2CF_LOG_LEVEL_DEBUG 4
#define E2CF_LOG_LEVEL_TRACE 5

/* Module bits (g_dbg_mask). */
#define DBG_M_MAIN  (1UL << 0)
#define DBG_M_ETH   (1UL << 1) /* EQOS bring-up, link, rings */
#define DBG_M_CAN   (1UL << 2) /* FlexCAN init/IRQ/MB */
#define DBG_M_PROTO (1UL << 3) /* E2CF parse/encode */
#define DBG_M_AGG   (1UL << 4) /* aggregation/flush decisions */
#define DBG_M_CFG   (1UL << 5) /* CFG_REQ/RSP handling */
#define DBG_M_HB    (1UL << 6) /* heartbeat / link supervision */
#define DBG_M_STATS (1UL << 7) /* periodic statistics report */

extern volatile uint8_t g_dbg_level;  /* runtime severity gate */
extern volatile uint32_t g_dbg_mask;  /* runtime module gate */

void dbg_log_init(void);
/* Core emit; prefer the LOG_* macros. Safe from any context incl. ISR. */
void dbg_log_emit(uint8_t level, uint32_t module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/* Drain pending log bytes to the UART. Main-loop context only. Bounded:
 * writes at most `budget` bytes per call so it cannot stall the loop. */
void dbg_log_drain(uint32_t budget);
/* Dropped-line counter (ring overflow). */
uint32_t dbg_log_dropped(void);
void dbg_log_clear_dropped(void);

/* Runtime control. */
static inline void dbg_log_set_level(uint8_t level) { g_dbg_level = level; }
static inline void dbg_log_set_mask(uint32_t mask) { g_dbg_mask = mask; }

/* Non-blocking single-key console on the debug UART (main loop):
 *   '0'..'5' set level NONE..TRACE, 'm' cycle common masks, 's' stats now,
 *   '?' help. Returns true if a key was consumed. */
bool dbg_log_console_poll(void);

/* ---- filtered macros -------------------------------------------------- */
#define DBG_LOG_AT(lvl, mod, ...)                                         \
    do                                                                    \
    {                                                                     \
        if (((lvl) <= E2CF_LOG_BUILD_LEVEL) && ((lvl) <= g_dbg_level) &&  \
            (((mod) & g_dbg_mask) != 0UL))                                \
        {                                                                 \
            dbg_log_emit((uint8_t)(lvl), (mod), __VA_ARGS__);             \
        }                                                                 \
    } while (0)

#define LOG_ERR(mod, ...) DBG_LOG_AT(E2CF_LOG_LEVEL_ERROR, mod, __VA_ARGS__)
#define LOG_WRN(mod, ...) DBG_LOG_AT(E2CF_LOG_LEVEL_WARN, mod, __VA_ARGS__)
#define LOG_INF(mod, ...) DBG_LOG_AT(E2CF_LOG_LEVEL_INFO, mod, __VA_ARGS__)
#define LOG_DBG(mod, ...) DBG_LOG_AT(E2CF_LOG_LEVEL_DEBUG, mod, __VA_ARGS__)
#define LOG_TRC(mod, ...) DBG_LOG_AT(E2CF_LOG_LEVEL_TRACE, mod, __VA_ARGS__)

#endif /* DBG_LOG_H_ */
