/*
 * Copyright 2025, 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MCUX_CONFIG_H_
#define _MCUX_CONFIG_H_

#define CONFIG_CLOCK_RETRY_TIMES 0
#define CONFIG_CPU_CORTEX_M 1
#define CONFIG_FLASH_BASE_ADDRESS 0x00400000
#define CONFIG_FLASH_DEFAULT_APP_OFFSET 0x0
#define CONFIG_HAS_FLASH_LOAD_OFFSET 1
#define CONFIG_FLASH_LOAD_OFFSET 0x0
#define CONFIG_FLEXCAN_POLLING_TIMEOUT 0
#define CONFIG_FLEXCAN_MODULE_TIMEOUT 1000000
#define CONFIG_UART_RETRY_TIMES 0

/*
 * The D-Cache is enabled (board.c: SCB_EnableDCache) and the Synopsys ENET_QOS
 * RX DMA data buffers live in cacheable SRAM. Enable the SDK's ENET cache
 * maintenance so received buffers are invalidated before the CPU reads them.
 * TX is not affected here: all E2CF transmit faces live in the DTCM
 * non-cacheable region (see eth_raw.c), so no clean is needed on send.
 */
#define FSL_ETH_ENABLE_CACHE_CONTROL 1

#endif /* _MCUX_CONFIG_H_ */
