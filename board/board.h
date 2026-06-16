/*
 * Copyright 2025 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BOARD_H_
#define BOARD_H_

#include "clock_config.h"
#include "fsl_common.h"
#include "fsl_siul2.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
/*! @brief The board name */
#define BOARD_NAME "FRDM-MCXE31B"

/*! @brief The UART to use for debug messages. */
#define BOARD_DEBUG_UART_TYPE kSerialPort_Uart

#define BOARD_DEBUG_UART_BASEADDR (uint32_t) LPUART_5
#define BOARD_DEBUG_UART_INSTANCE 5U

#define BOARD_DEBUG_UART_CLK_FREQ CLOCK_GetFreq(kCLOCK_Lpuart5Clk)

#ifndef BOARD_DEBUG_UART_BAUDRATE
#define BOARD_DEBUG_UART_BAUDRATE 115200
#endif /* BOARD_DEBUG_UART_BAUDRATE */

/*! @brief The EMAC PHY address. */
#define BOARD_EMAC_PHY_ADDRESS (0x0U) /* Phy address of EMAC port. */

/*******************************************************************************
 * API
 ******************************************************************************/
#if defined(__cplusplus)
extern "C" {

#endif /* __cplusplus */
/*! @brief Init the debug console. */
void BOARD_InitDebugConsole(void);
/*! @brief Config the MPU. */
void BOARD_ConfigMPU(void);
void BOARD_ClockPreConfig(void);
void BOARD_ClockPostConfig(void);
void BOARD_InitHardware(void);

#if defined(__cplusplus)
}
#endif /* __cplusplus */
#endif /* BOARD_H_ */
