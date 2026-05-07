/*
 * uart_smoke.c - Shared smoke test for LPUART0 and LPUART3
 *
 * TX: polling via LPUART_WriteBlocking(), sends counter string every 1 s
 * RX: interrupt-driven ring buffer; bytes are echoed back on the same port
 *
 * Pin assignment (configure SIUL2 in BOARD_InitRS485UARTPins):
 *   UART0: PTA3 = TX, PTA2 = RX
 *   UART3: PTD2 = TX, PTD3 = RX
 *
 * RS485 direction control is assumed to be handled externally by hardware.
 */

#include "uart_smoke.h"

#include <stdbool.h>
#include <stdio.h>

#include "board.h"
#include "fsl_clock.h"
#include "fsl_debug_console.h"
#include "fsl_lpuart.h"

/*******************************************************************************
 * Configuration
 ******************************************************************************/
#define UART_SMOKE_BAUDRATE            115200U
#define UART_SMOKE_TX_PERIOD_MS        1000U
#define UART_SMOKE_RX_RING_SIZE        256U
#define UART_SMOKE_MAX_DRAIN_PER_POLL  32U

/*******************************************************************************
 * Types
 ******************************************************************************/
typedef struct
{
    LPUART_Type *base;
    IRQn_Type irq;
    clock_name_t clockName;
    const char *name;
    const char *txPin;
    const char *rxPin;

    volatile uint8_t rxBuf[UART_SMOKE_RX_RING_SIZE];
    volatile uint16_t rxHead;
    volatile uint16_t rxTail;

    uint32_t lastTxMs;
    uint32_t txCounter;
} uart_smoke_channel_t;

/*******************************************************************************
 * Module-private state
 ******************************************************************************/
static uart_smoke_channel_t s_uartChannels[] =
{
    {
        .base = LPUART_0,
        .irq = LPUART_0_IRQn,
        .clockName = kCLOCK_Lpuart0Clk,
        .name = "UART0",
        .txPin = "PTA3",
        .rxPin = "PTA2",
    },
    {
        .base = LPUART_3,
        .irq = LPUART_3_IRQn,
        .clockName = kCLOCK_Lpuart3Clk,
        .name = "UART3",
        .txPin = "PTD2",
        .rxPin = "PTD3",
    },
};

/*******************************************************************************
 * Helpers
 ******************************************************************************/
static void uart_smoke_store_rx_byte(uart_smoke_channel_t *channel)
{
    if ((LPUART_GetStatusFlags(channel->base) & kLPUART_RxDataRegFullFlag) != 0U)
    {
        uint8_t data = LPUART_ReadByte(channel->base);
        uint16_t next = (uint16_t)((channel->rxHead + 1U) % UART_SMOKE_RX_RING_SIZE);
        if (next != channel->rxTail)
        {
            channel->rxBuf[channel->rxHead] = data;
            channel->rxHead = next;
        }
    }
}

static void uart_smoke_init_one(uart_smoke_channel_t *channel)
{
    lpuart_config_t cfg;

    LPUART_GetDefaultConfig(&cfg);
    cfg.baudRate_Bps = UART_SMOKE_BAUDRATE;
    cfg.enableTx = true;
    cfg.enableRx = true;
    LPUART_Init(channel->base, &cfg, CLOCK_GetFreq(channel->clockName));

    LPUART_EnableInterrupts(channel->base, kLPUART_RxDataRegFullInterruptEnable);
    NVIC_SetPriority(channel->irq, 3U);
    (void)EnableIRQ(channel->irq);

    PRINTF("[%s] init  %ubps  %s=TX  %s=RX\r\n",
           channel->name,
           UART_SMOKE_BAUDRATE,
           channel->txPin,
           channel->rxPin);
}

static void uart_smoke_poll_one(uart_smoke_channel_t *channel, uint32_t ms)
{
    if ((ms - channel->lastTxMs) >= UART_SMOKE_TX_PERIOD_MS)
    {
        char buf[40];
        int len;

        channel->lastTxMs = ms;
        len = snprintf(buf, sizeof(buf), "%s cnt=%lu\r\n",
                       channel->name,
                       (unsigned long)channel->txCounter++);
        if (len > 0)
        {
            size_t txLen = ((size_t)len < sizeof(buf)) ? (size_t)len : (sizeof(buf) - 1U);
            LPUART_WriteBlocking(channel->base, (const uint8_t *)buf, txLen);
        }
    }

    for (uint32_t drained = 0U;
         (channel->rxTail != channel->rxHead) && (drained < UART_SMOKE_MAX_DRAIN_PER_POLL);
         drained++)
    {
        uint8_t byte = channel->rxBuf[channel->rxTail];
        channel->rxTail = (uint16_t)((channel->rxTail + 1U) % UART_SMOKE_RX_RING_SIZE);

        LPUART_WriteBlocking(channel->base, &byte, 1U);
        PRINTF("[%s] RX: 0x%02X '%c'\r\n",
               channel->name,
               byte,
               (byte >= 0x20U && byte < 0x7FU) ? (char)byte : '.');
    }
}

/*******************************************************************************
 * Strong IRQ handlers
 ******************************************************************************/
void LPUART_0_IRQHandler(void)
{
    uart_smoke_store_rx_byte(&s_uartChannels[0]);
    SDK_ISR_EXIT_BARRIER;
}

void LPUART_3_IRQHandler(void)
{
    uart_smoke_store_rx_byte(&s_uartChannels[1]);
    SDK_ISR_EXIT_BARRIER;
}

/*******************************************************************************
 * Public API
 ******************************************************************************/
void uart_smoke_init_all(void)
{
    for (uint32_t i = 0U; i < (sizeof(s_uartChannels) / sizeof(s_uartChannels[0])); i++)
    {
        uart_smoke_init_one(&s_uartChannels[i]);
    }
}

void uart_smoke_poll_all(uint32_t ms)
{
    for (uint32_t i = 0U; i < (sizeof(s_uartChannels) / sizeof(s_uartChannels[0])); i++)
    {
        uart_smoke_poll_one(&s_uartChannels[i], ms);
    }
}
