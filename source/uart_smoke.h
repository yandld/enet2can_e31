/*
 * uart_smoke.h - Shared LPUART0/LPUART3 smoke test entry points
 */
#ifndef UART_SMOKE_H_
#define UART_SMOKE_H_

#include <stdint.h>

void uart_smoke_init_all(void);
void uart_smoke_poll_all(uint32_t ms);

#endif /* UART_SMOKE_H_ */
