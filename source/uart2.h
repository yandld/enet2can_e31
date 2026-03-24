/*
 * uart2.h — LPUART2 demo (MIKROE connector)
 *   TX: PTE12, polling, 1 s period
 *   RX: PTD17, interrupt, echoed to debug console
 */
#ifndef UART2_H_
#define UART2_H_

#include <stdint.h>

void uart2_init(void);
void uart2_poll(uint32_t ms);

#endif /* UART2_H_ */
