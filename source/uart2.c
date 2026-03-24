/*
 * uart2.c — LPUART2 Demo (MIKROE connector)
 *
 * TX: polling via LPUART_WriteBlocking(), sends counter string every 1 s
 * RX: interrupt-driven ring buffer; received bytes echoed to debug console
 *
 * Pin assignment (configure SIUL2 in BOARD_InitMIKROEUARTPins):
 *   PTE12 — LPUART2_TX-MIKROE
 *   PTD17 — LPUART2_RX-MIKROE
 */

#include "uart2.h"
#include <stdio.h>
#include "fsl_lpuart.h"
#include "fsl_debug_console.h"
#include "board.h"

/*******************************************************************************
 * Configuration
 ******************************************************************************/
#define UART2_BAUDRATE      115200U
#define UART2_TX_PERIOD_MS  1000U
#define UART2_RX_RING_SIZE  256U

/*******************************************************************************
 * Module-private state
 ******************************************************************************/
static volatile uint8_t  s_rxBuf[UART2_RX_RING_SIZE];
static volatile uint16_t s_rxHead;   /* written by ISR */
static volatile uint16_t s_rxTail;   /* read by uart2_poll */

static uint32_t s_lastTxMs;
static uint32_t s_txCounter;

/*******************************************************************************
 * LPUART2 ISR — store each received byte in the ring buffer
 ******************************************************************************/
void LPUART_2_IRQHandler(void)
{
    if ((LPUART_GetStatusFlags(LPUART_2) & kLPUART_RxDataRegFullFlag) != 0U)
    {
        uint8_t  data = LPUART_ReadByte(LPUART_2);
        uint16_t next = (uint16_t)((s_rxHead + 1U) % UART2_RX_RING_SIZE);
        if (next != s_rxTail)           /* drop on overflow */
        {
            s_rxBuf[s_rxHead] = data;
            s_rxHead          = next;
        }
    }
    SDK_ISR_EXIT_BARRIER;
}

/*******************************************************************************
 * Public API
 ******************************************************************************/
void uart2_init(void)
{
    lpuart_config_t cfg;
    LPUART_GetDefaultConfig(&cfg);
    cfg.baudRate_Bps = UART2_BAUDRATE;
    cfg.enableTx     = true;
    cfg.enableRx     = true;
    LPUART_Init(LPUART_2, &cfg, CLOCK_GetFreq(kCLOCK_Lpuart2Clk));

    /* Enable RX interrupt */
    LPUART_EnableInterrupts(LPUART_2, kLPUART_RxDataRegFullInterruptEnable);
    NVIC_SetPriority(LPUART_2_IRQn, 3U);
    (void)EnableIRQ(LPUART_2_IRQn);

    PRINTF("[UART2] init  %ubps  PTE12=TX  PTD17=RX\r\n", UART2_BAUDRATE);
}

void uart2_poll(uint32_t ms)
{
    /* --- Periodic TX (polling) --- */
    if ((ms - s_lastTxMs) >= UART2_TX_PERIOD_MS)
    {
        s_lastTxMs = ms;
        char   buf[40];
        int    len = snprintf(buf, sizeof(buf), "UART2 cnt=%lu\r\n",
                              (unsigned long)s_txCounter++);
        LPUART_WriteBlocking(LPUART_2, (const uint8_t *)buf, (size_t)len);
    }

    /* --- Drain RX ring buffer, echo back on UART2 --- */
    while (s_rxTail != s_rxHead)
    {
        uint8_t byte = s_rxBuf[s_rxTail];
        s_rxTail = (uint16_t)((s_rxTail + 1U) % UART2_RX_RING_SIZE);
        LPUART_WriteBlocking(LPUART_2, &byte, 1U);
        PRINTF("[UART2] RX: 0x%02X '%c'\r\n",
               byte,
               (byte >= 0x20U && byte < 0x7FU) ? (char)byte : '.');
    }
}
