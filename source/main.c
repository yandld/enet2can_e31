/*
 * main.c - CAN0-first CAN FD Ethernet gateway bring-up
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "arch/sys_arch.h"
#include "board.h"
#include "can_gateway_protocol.h"
#include "can_service.h"
#include "can_udp_gateway.h"
#include "ethernet_lwip.h"
#include "fsl_debug_console.h"
#include "gateway_router.h"

void SysTick_Handler(void)
{
    can_service_tick_1ms();
    time_isr();
}

int main(void)
{
    BOARD_InitHardware();
    SysTick_Config(SystemCoreClock / 1000U);

    PRINTF("\r\n========================================\r\n");
    PRINTF("  CAN0-first CAN FD gateway  -  MCXE31B\r\n");
    PRINTF("========================================\r\n");
    PRINTF("  Default active CAN mask: 0x%x\r\n", (unsigned)CAN_ACTIVE_MASK);
    PRINTF("  Gateway protocol: magic=0x%x data_port=%u status_port=%u\r\n",
           (unsigned)CAN_GATEWAY_MAGIC,
           CAN_GATEWAY_UDP_DATA_PORT,
           CAN_GATEWAY_UDP_STATUS_PORT);
    PRINTF("========================================\r\n\r\n");

    (void)ethernet_lwip_init();
    (void)can_service_init(CAN_ACTIVE_MASK);
    (void)gateway_router_init(CAN_ACTIVE_MASK);
    (void)can_udp_gateway_init();

    while (1)
    {
        ethernet_lwip_poll();
        can_service_poll();
        can_udp_gateway_poll();
    }
}
