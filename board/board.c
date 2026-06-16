/*
 * Copyright 2025, 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "board.h"
#include "fsl_debug_console.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*******************************************************************************
 * Variables
 ******************************************************************************/

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

/*******************************************************************************
 * Code
 ******************************************************************************/
void BOARD_ConfigMPU(void)
{
#if defined(SDK_SRAM_NONCACHE_SECTION) /* If Non-cacheable section is located in SRAM. */
#if defined(__CC_ARM) || defined(__ARMCC_VERSION)
    extern uint32_t Image$$RW_m_ncache$$Base[];
    /* RW_m_ncache_unused is a auxiliary region which is used to get the whole size of noncache section */
    extern uint32_t Image$$RW_m_ncache_unused$$Base[];
    extern uint32_t Image$$RW_m_ncache_unused$$ZI$$Limit[];
    extern uint32_t Image$$RW_m_ncache_aux$$Base[];
    uint32_t nonCacheStart = (uint32_t)Image$$RW_m_ncache$$Base;
    uint32_t nonCacheSize  = ((uint32_t)Image$$RW_m_ncache_unused$$Base == nonCacheStart) ?
                                 0 :
                                 ((uint32_t)Image$$RW_m_ncache_unused$$ZI$$Limit - nonCacheStart);
#elif defined(__MCUXPRESSO)
    extern uint32_t __base_NCACHE_REGION;
    extern uint32_t __top_NCACHE_REGION;
    uint32_t nonCacheStart = (uint32_t)(&__base_NCACHE_REGION);
    uint32_t nonCacheSize  = (uint32_t)(&__top_NCACHE_REGION) - nonCacheStart;
#elif defined(__ICCARM__) || defined(__GNUC__)
    extern uint32_t __NCACHE_REGION_START[];
    extern uint32_t __NCACHE_REGION_SIZE[];
    uint32_t nonCacheStart = (uint32_t)__NCACHE_REGION_START;
    uint32_t nonCacheSize  = (uint32_t)__NCACHE_REGION_SIZE;
#endif

    volatile uint32_t i;
#endif

   /* Disable I cache and D cache */
    if (SCB_CCR_IC_Msk == (SCB_CCR_IC_Msk & SCB->CCR))
    {
        SCB_DisableICache();
    }
    if (SCB_CCR_DC_Msk == (SCB_CCR_DC_Msk & SCB->CCR))
    {
        SCB_DisableDCache();
    }

    /* Disable MPU */
    ARM_MPU_Disable();

    /* clang-format off */

    /* MPU configure:
     * Use ARM_MPU_RASR(DisableExec, AccessPermission, TypeExtField, IsShareable, IsCacheable, IsBufferable, SubRegionDisable, Size)
     * API in mpu_armv7.h.
     * param DisableExec       Instruction access (XN) disable bit,0=instruction fetches enabled, 1=instruction fetches disabled.
     * param AccessPermission  Data access permissions, allows you to configure read/write access for User and Privileged mode.
     *                         Use MACROS defined in mpu_armv7.h:
     *                         ARM_MPU_AP_NONE/ARM_MPU_AP_PRIV/ARM_MPU_AP_URO/ARM_MPU_AP_FULL/ARM_MPU_AP_PRO/ARM_MPU_AP_RO
     *
     * Combine TypeExtField/IsShareable/IsCacheable/IsBufferable to configure MPU memory access attributes.
     *  TypeExtField  IsShareable  IsCacheable  IsBufferable   Memory Attribute    Shareability        Cache
     *     0             x           0           0             Strongly Ordered    shareable
     *     0             x           0           1              Device             shareable
     *     0             0           1           0              Normal             not shareable   Outer and inner write
     *                                                                                             through no write allocate
     *     0             0           1           1              Normal             not shareable   Outer and inner write
     *                                                                                             back no write allocate
     *     0             1           1           0              Normal             shareable       Outer and inner write
     *                                                                                             through no write allocate
     *     0             1           1           1              Normal             shareable       Outer and inner write
     *                                                                                             back no write allocate
     *     1             0           0           0              Normal             not shareable   outer and inner
     *                                                                                             noncache
     *     1             1           0           0              Normal             shareable       outer and inner
     *                                                                                             noncache
     *     1             0           1           1              Normal             not shareable   outer and inner write
     *                                                                                             back write/read acllocate
     *     1             1           1           1              Normal             shareable       outer and inner write
     *                                                                                             back write/read acllocate
     *     2             x           0           0              Device             not shareable
     *   Above are normal use settings, if your want to see more details or want to config different inner/outer cache
     * policy, please refer to Table 4-55 /4-56 in arm cortex-M7 generic user guide <dui0646b_cortex_m7_dgug.pdf>
     *
     * param SubRegionDisable  Sub-region disable field. 0=sub-region is enabled, 1=sub-region is disabled.
     * param Size              Region size of the region to be configured. use ARM_MPU_REGION_SIZE_xxx MACRO in mpu_armv7.h.
     */

    /* clang-format on */

    /*
     * Add default region to deny access to whole address space to workaround speculative prefetch.
     * Refer to Arm errata 1013783-B for more details.
     */
    /* Region 0 setting: Instruction access disabled, No data access permission. */
    MPU->RBAR = ARM_MPU_RBAR(0, 0x00000000U);
    MPU->RASR = ARM_MPU_RASR(1, ARM_MPU_AP_NONE, 0, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_4GB);

    /* Region 1 setting: Memory with Normal type, not shareable, outer/inner noncache, ITCM */
    MPU->RBAR = ARM_MPU_RBAR(1, 0x00000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 1, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_32KB);

    /* Region 2 setting: Memory with Normal type, not shareable, outer/inner write back, Flash */
    MPU->RBAR = ARM_MPU_RBAR(2, 0x00400000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 0, 0, 1, 1, 0, ARM_MPU_REGION_SIZE_4MB);
    
    /* Region 3 setting: Memory with Normal type, not shareable, outer/inner write back, DFlash */
    MPU->RBAR = ARM_MPU_RBAR(3, 0x10000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 0, 0, 1, 1, 0, ARM_MPU_REGION_SIZE_128KB);

    /* Region 4 setting: Memory with Normal type, not shareable, outer/inner noncache, ITCM backdoor */
    MPU->RBAR = ARM_MPU_RBAR(4, 0x11000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 1, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_32KB);

    /* Region 5 setting: Memory with Normal type, not shareable, outer/inner noncache, ITCM1 backdoor */
    MPU->RBAR = ARM_MPU_RBAR(5, 0x11400000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 1, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_32KB);

    /* Region 6 setting: Memory with Normal type, not shareable, outer/inner noncache, DTCM */
    MPU->RBAR = ARM_MPU_RBAR(6, 0x20000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 1, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_64KB);

    /* Region 7 setting: Memory with Normal type, not shareable, outer/inner write back, SRAM */
    MPU->RBAR = ARM_MPU_RBAR(7, 0x20400000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 0, 0, 1, 1, 0, ARM_MPU_REGION_SIZE_512KB);

    /* Region 8 setting: Memory with Normal type, not shareable, outer/inner noncache, DTCM backdoor */
    MPU->RBAR = ARM_MPU_RBAR(8, 0x21000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 1, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_64KB);
    
    /* Region 9 setting: Memory with Normal type, not shareable, outer/inner noncache, DTCM1 backdoor */
    MPU->RBAR = ARM_MPU_RBAR(9, 0x21400000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 1, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_64KB);

    /* Region 10 setting: Memory with Device type, not shareable, non-cacheable. */
    MPU->RBAR = ARM_MPU_RBAR(10, 0x40000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 2, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_512MB);

    /* Region 11 setting: Memory with Device type, not shareable, non-cacheable, QSPI RX buffer. */
    MPU->RBAR = ARM_MPU_RBAR(11, 0x67000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 2, 0, 0, 0, 0, ARM_MPU_REGION_SIZE_1KB);

#if defined(SDK_USE_QSPI) /* Only configure QSPI memory when used. Refer to Arm errata 1013783-B */
    /* Region 12 setting: Memory with Normal type, not shareable, outer/inner write back, QSPI AHB */
    MPU->RBAR = ARM_MPU_RBAR(12, 0x68000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 0, 0, 0, 1, 0, ARM_MPU_REGION_SIZE_128MB);
#endif

#if defined(SDK_SRAM_NONCACHE_SECTION)
    i = 0;
    while ((nonCacheSize >> i) > 0x1U)
    {
        i++;
    }

    if (i != 0)
    {
        /* The MPU region size should be 2^N, 5<=N<=32, region base should be multiples of size. */
        assert(!(nonCacheStart % nonCacheSize));
        assert(nonCacheSize == (uint32_t)(1 << i));
        assert(i >= 5);

        /* Region 13 setting: Memory with Normal type, not shareable, non-cacheable */
        MPU->RBAR = ARM_MPU_RBAR(13, nonCacheStart);
        MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 1, 0, 0, 0, 0, i - 1);
    }
#endif
    
    /* Region 14 setting: with Normal type, not shareable, outer/inner write back. */
    MPU->RBAR = ARM_MPU_RBAR(14, 0x1B000000U);
    MPU->RASR = ARM_MPU_RASR(0, ARM_MPU_AP_FULL, 0, 0, 1, 1, 0, ARM_MPU_REGION_SIZE_8KB);

    /* Enable MPU */
    ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_HFNMIENA_Msk);

    /* Enable I cache and D cache */
    SCB_EnableDCache();
    SCB_EnableICache();
}

/* Bring up the LPUART debug console at the board default baud rate. */
void BOARD_InitDebugConsole(void)
{
    uint32_t uartClkSrcFreq;
    
    uartClkSrcFreq = BOARD_DEBUG_UART_CLK_FREQ;

    DbgConsole_Init(BOARD_DEBUG_UART_INSTANCE, BOARD_DEBUG_UART_BAUDRATE, BOARD_DEBUG_UART_TYPE, uartClkSrcFreq);
}

/* Don't access system RAM when configuring PRAM FT_DIS.  */
AT_QUICKACCESS_SECTION_CODE(void BOARD_EnableSRAMExtraLatency(bool en))
{
    if (en)
    {
        /* Configure SRAM read wait states. */
        PRAMC_0->PRCR1 |= PRAMC_PRCR1_FT_DIS_MASK;
#if defined(PRAMC_1)
        PRAMC_1->PRCR1 |= PRAMC_PRCR1_FT_DIS_MASK;
#endif
    }
    else
    {
        PRAMC_0->PRCR1 &= ~PRAMC_PRCR1_FT_DIS_MASK;
#if defined(PRAMC_1)
        PRAMC_1->PRCR1 &= ~PRAMC_PRCR1_FT_DIS_MASK;
#endif
    }
}

/* Pre-PLL safe clock selection, last-mile regulator setup and SRAM
 * wait-state programming before the clock tree switches. */
void BOARD_ClockPreConfig(void)
{
    CLOCK_SelectSafeClock(kFIRC_CLK_to_MUX0);
#if defined(FSL_FEATURE_PMC_HAS_LAST_MILE_REGULATOR) && (FSL_FEATURE_PMC_HAS_LAST_MILE_REGULATOR)
    /* Enables PMC last mile regulator before enable PLL.  */
    if ((PMC->LVSC & PMC_LVSC_LVD15S_MASK) != 0U)
    {
        /* External bipolar junction transistor is connected between external voltage and V15 input pin. */
        PMC->CONFIG |= PMC_CONFIG_LMBCTLEN_MASK;
    }
    while((PMC->LVSC & PMC_LVSC_LVD15S_MASK) != 0U)
    {
    }
    PMC->CONFIG |= PMC_CONFIG_LMEN_MASK;
    while((PMC->CONFIG & PMC_CONFIG_LMSTAT_MASK) == 0u)
    {
    }
#endif /* FSL_FEATURE_PMC_HAS_LAST_MILE_REGULATOR */
    BOARD_EnableSRAMExtraLatency(true);
}

/* Post-clock-tree fixups (MUX0 divider immediate-update trigger). */
void BOARD_ClockPostConfig(void)
{
    /* Change MUX0 DIV trigger type to Immediate update. */
    CLOCK_SetClkMux0DivTriggerType(KCLOCK_ImmediateUpdate);
}
