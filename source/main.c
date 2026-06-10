/*
 * main.c - CAN0-first CAN FD Ethernet gateway bring-up
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>

#include "arch/sys_arch.h"
#include "board.h"
#include "can_gateway_protocol.h"
#include "can_service.h"
#include "can_udp_gateway.h"
#include "ethernet_lwip.h"
#include "fsl_debug_console.h"
#include "gateway_router.h"
#include "latency_timer.h"
#include "status_led.h"

void SysTick_Handler(void)
{
    can_service_tick_1ms();
    time_isr();
}

static void fault_log_and_halt(const char *name, const uint32_t *stacked, uint32_t excReturn)
{
    uint32_t r0 = (stacked != NULL) ? stacked[0] : 0U;
    uint32_t r1 = (stacked != NULL) ? stacked[1] : 0U;
    uint32_t r2 = (stacked != NULL) ? stacked[2] : 0U;
    uint32_t r3 = (stacked != NULL) ? stacked[3] : 0U;
    uint32_t r12 = (stacked != NULL) ? stacked[4] : 0U;
    uint32_t lr = (stacked != NULL) ? stacked[5] : 0U;
    uint32_t pc = (stacked != NULL) ? stacked[6] : 0U;
    uint32_t xpsr = (stacked != NULL) ? stacked[7] : 0U;

    PRINTF("\r\nFault: %s\r\n", name);
    PRINTF("  stacked r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\r\n",
           (unsigned)r0,
           (unsigned)r1,
           (unsigned)r2,
           (unsigned)r3);
    PRINTF("  stacked r12=0x%08x lr=0x%08x pc=0x%08x xpsr=0x%08x\r\n",
           (unsigned)r12,
           (unsigned)lr,
           (unsigned)pc,
           (unsigned)xpsr);
    PRINTF("  exc_return=0x%08x\r\n", (unsigned)excReturn);
    PRINTF("  SCB CFSR=0x%08x HFSR=0x%08x DFSR=0x%08x AFSR=0x%08x\r\n",
           (unsigned)SCB->CFSR,
           (unsigned)SCB->HFSR,
           (unsigned)SCB->DFSR,
           (unsigned)SCB->AFSR);
    PRINTF("  SCB MMFAR=0x%08x BFAR=0x%08x SHCSR=0x%08x\r\n",
           (unsigned)SCB->MMFAR,
           (unsigned)SCB->BFAR,
           (unsigned)SCB->SHCSR);

    status_led_fault();

    while (1)
    {
        __asm volatile ("nop");
    }
}

static void fault_log_hardfault(uint32_t *stacked, uint32_t excReturn) __attribute__((used));
static void fault_log_hardfault(uint32_t *stacked, uint32_t excReturn)
{
    fault_log_and_halt("HardFault", stacked, excReturn);
}

static void fault_log_memmanage(uint32_t *stacked, uint32_t excReturn) __attribute__((used));
static void fault_log_memmanage(uint32_t *stacked, uint32_t excReturn)
{
    fault_log_and_halt("MemManage", stacked, excReturn);
}

static void fault_log_busfault(uint32_t *stacked, uint32_t excReturn) __attribute__((used));
static void fault_log_busfault(uint32_t *stacked, uint32_t excReturn)
{
    fault_log_and_halt("BusFault", stacked, excReturn);
}

static void fault_log_usagefault(uint32_t *stacked, uint32_t excReturn) __attribute__((used));
static void fault_log_usagefault(uint32_t *stacked, uint32_t excReturn)
{
    fault_log_and_halt("UsageFault", stacked, excReturn);
}

/* Fault wrappers preserve the original exception stack pointer before C prologue. */
#define FAULT_LOG_WRAPPER(handler, logger)           \
    void handler(void) __attribute__((naked));       \
    void handler(void)                               \
    {                                                \
        __asm volatile                               \
        (                                            \
            "tst lr, #4\n"                           \
            "ite eq\n"                               \
            "mrseq r0, msp\n"                        \
            "mrsne r0, psp\n"                        \
            "mov r1, lr\n"                           \
            "b " #logger "\n"                        \
        );                                           \
    }

FAULT_LOG_WRAPPER(HardFault_Handler, fault_log_hardfault)
FAULT_LOG_WRAPPER(MemManage_Handler, fault_log_memmanage)
FAULT_LOG_WRAPPER(BusFault_Handler, fault_log_busfault)
FAULT_LOG_WRAPPER(UsageFault_Handler, fault_log_usagefault)

int main(void)
{
    BOARD_InitHardware();
    SysTick_Config(SystemCoreClock / 1000U);
    latency_timer_init();

    PRINTF("\r\n========================================\r\n");
    PRINTF("  CAN0-first CAN FD gateway  -  MCXE31B\r\n");
    PRINTF("========================================\r\n");
    PRINTF("  Default active CAN mask: 0x%x\r\n", (unsigned)CAN_ACTIVE_MASK);
    PRINTF("  Gateway protocol: magic=0x%x data_port=%u control_port=%u\r\n",
           (unsigned)CAN_GATEWAY_MAGIC,
           CAN_GATEWAY_UDP_DATA_PORT,
           CAN_GATEWAY_UDP_CONTROL_PORT);
    PRINTF("========================================\r\n\r\n");

    (void)ethernet_lwip_init();
    (void)can_service_init(CAN_ACTIVE_MASK);
    (void)gateway_router_init(CAN_ACTIVE_MASK);
    (void)can_udp_gateway_init();
    status_led_init();

    while (1)
    {
        uint32_t t0, t1, t2, t3;

        can_udp_gateway_mark_loop();
        t0 = latency_cycle_now();
        can_udp_gateway_mark_eth_rx(t0); /* MAC-RX-pull instant: origin for eth-to-eth latency */
        ethernet_lwip_poll();
        t1 = latency_cycle_now();
        can_service_poll();
        t2 = latency_cycle_now();
        can_udp_gateway_poll();
        t3 = latency_cycle_now();
        can_udp_gateway_mark_legs(t1 - t0, t2 - t1, t3 - t2);

        status_led_poll();
    }
}
