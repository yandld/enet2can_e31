/*
 * main.c - E2CF gateway firmware, bare-metal superloop (MCXE31B)
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Architecture (design doc 03 §2.2): the data plane lives in tiered NVIC
 * interrupts (CAN RX/TXC prio 1, EMAC RX prio 2); the superloop only runs
 * non-deadline work - T_agg flush checks, HB/EVT/TIME ticks, error polling,
 * link supervision, statistics and deferred log draining.
 */
#include <stdint.h>

#include "board.h"
#include "can_hw.h"
#include "dbg_log.h"
#include "e2cf_config.h"
#include "e2cf_core.h"
#include "e2cf_proto.h"
#include "eth_raw.h"
#include "fsl_debug_console.h"
#include "gw_time.h"
#include "status_led.h"

static volatile uint32_t s_ms;

/*
 * 1 ms tick: advance the coarse millisecond counter and run the DWT wrap
 * tracker.
 */
void SysTick_Handler(void)
{
    s_ms++;
    gw_time_track_wrap();
}

/* ---- fault logging (kept from the donor project: prints stacked regs) ---- */

static void fault_log_and_halt(const char *name, const uint32_t *stacked, uint32_t exc_return)
{
    PRINTF("\r\nFault: %s\r\n", name);
    if (stacked != NULL)
    {
        PRINTF("  r0=%08lx r1=%08lx r2=%08lx r3=%08lx\r\n",
               (unsigned long)stacked[0], (unsigned long)stacked[1],
               (unsigned long)stacked[2], (unsigned long)stacked[3]);
        PRINTF("  r12=%08lx lr=%08lx pc=%08lx xpsr=%08lx\r\n",
               (unsigned long)stacked[4], (unsigned long)stacked[5],
               (unsigned long)stacked[6], (unsigned long)stacked[7]);
    }
    PRINTF("  exc_return=%08lx CFSR=%08lx HFSR=%08lx BFAR=%08lx\r\n",
           (unsigned long)exc_return, (unsigned long)SCB->CFSR,
           (unsigned long)SCB->HFSR, (unsigned long)SCB->BFAR);
    status_led_fault();
    while (1)
    {
        __asm volatile("nop");
    }
}

/* C-level HardFault sink: print the stacked register frame and halt. */
static void fault_log_hardfault(uint32_t *stacked, uint32_t exc_return) __attribute__((used));
static void fault_log_hardfault(uint32_t *stacked, uint32_t exc_return)
{
    fault_log_and_halt("HardFault", stacked, exc_return);
}

/*
 * Naked entry: select MSP/PSP from EXC_RETURN bit 2 and branch to the C sink.
 */
void HardFault_Handler(void) __attribute__((naked));
void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b fault_log_hardfault\n");
}

/* ---- main ----------------------------------------------------------------*/

/*
 * Board/eth/can/core bring-up followed by the superloop: data-plane
 * housekeeping every iteration, error polling at 10 ms, link polling at 100
 * ms, deferred log drain and debug console.
 */
int main(void)
{
    uint32_t last_err_poll_ms = 0U;
    uint32_t last_link_poll_ms = 0U;
#if E2CF_STATS_PERIOD_MS != 0U
    uint32_t last_stats_ms = 0U;
#endif

    BOARD_InitHardware();
    status_led_init();
    gw_time_init();
    dbg_log_init();

    SysTick_Config(SystemCoreClock / 1000U);
    NVIC_SetPriority(SysTick_IRQn, E2CF_IRQPRIO_SYSTICK);

    PRINTF("\r\n=============================================\r\n");
    PRINTF("  E2CF gateway  -  MCXE31B  (fw %08lx)\r\n", (unsigned long)E2CF_FW_VERSION);
    PRINTF("  build " __DATE__ " " __TIME__ "\r\n");
    PRINTF("  proto v%u  ethertype 0x%04x  win %u  T_agg %uus\r\n",
           E2CF_VERSION, E2CF_ETHERTYPE, E2CF_WIN_DEPTH, E2CF_T_AGG_US);
    PRINTF("  chan mask 0x%02x  autostart %u  loopback %u\r\n",
           E2CF_ACTIVE_CHAN_MASK, E2CF_AUTOSTART_CHANNELS, E2CF_AUTOSTART_LOOPBACK);
    PRINTF("  dbg: keys 0-5=level m=mask s=stats p=profile ?=help\r\n");
    PRINTF("=============================================\r\n\r\n");

    /* Core aggregation state (face=-1 sentinels) must be reset BEFORE any
     * data-plane IRQ or CAN autostart can fire an upcall against it. */
    (void)e2cf_core_init();

    if (!eth_raw_init())
    {
        PRINTF("FATAL: ethernet init failed\r\n");
        status_led_fault();
        while (1) {}
    }
    if (!can_hw_init())
    {
        PRINTF("FATAL: can init failed\r\n");
        status_led_fault();
        while (1) {}
    }

    while (1)
    {
        uint32_t ms = s_ms;

        /* Data-plane housekeeping with sub-us resolution. */
        e2cf_core_poll();
        status_led_poll(ms);

        /* 10 ms: CAN error/fault supervision + stuck-TX watchdog. */
        if ((ms - last_err_poll_ms) >= 10U)
        {
            last_err_poll_ms = ms;
            can_hw_poll_errors();
        }

        /* 100 ms: PHY link state. */
        if ((ms - last_link_poll_ms) >= 100U)
        {
            last_link_poll_ms = ms;
            eth_raw_poll_link();
        }

        /* Deferred log drain (bounded) + runtime debug console. */
        dbg_log_drain(256U);
        if (dbg_log_console_poll())
        {
            /* 's' pressed: dump everything now. */
            eth_raw_dump_stats();
            can_hw_dump_stats();
            e2cf_core_dump_stats();
        }

#if E2CF_STATS_PERIOD_MS != 0U
        if ((ms - last_stats_ms) >= E2CF_STATS_PERIOD_MS)
        {
            last_stats_ms = ms;
            eth_raw_dump_stats();
            can_hw_dump_stats();
            e2cf_core_dump_stats();
        }
#endif
    }
}
