/*
 * Copyright 2025 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*${header:start}*/
#include "fsl_enet_qos.h"
#include "fsl_phylan8741.h"
#include "fsl_phy.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
/*${header:end}*/

/*${variable:start}*/
phy_lan8741_resource_t g_phy_resource;
/*${variable:end}*/

/*${function:start}*/
void BOARD_InitModuleClock(void)
{
    CLOCK_SetEmacRmiiTxClkFreq(50000000U);

    CLOCK_AttachClk(kEMAC_RMII_TX_CLK_to_EMAC_TX);
    CLOCK_AttachClk(kEMAC_RMII_TX_CLK_to_EMAC_RX);
    CLOCK_AttachClk(kEMAC_RMII_TX_CLK_to_EMAC_TS);

    CLOCK_SetClkDiv(kCLOCK_DivEmacRxClk, 2U);
    CLOCK_SetClkDiv(kCLOCK_DivEmacTxClk, 2U);
    CLOCK_SetClkDiv(kCLOCK_DivEmacTsClk, 2U);

    ENET_QOS_EnableClock(true);
}

void ENET_QOS_SetSYSControl(enet_qos_mii_mode_t miiMode)
{
    if (miiMode == kENET_QOS_RmiiMode)
    {
        DCM_GPR->DCMRWF1 = (DCM_GPR->DCMRWF1 & DCM_GPR_DCMRWF1_RMII_MII_SEL_MASK) |
                           DCM_GPR_DCMRWF1_RMII_MII_SEL(1U);
    }
    else
    {
        DCM_GPR->DCMRWF1 = (DCM_GPR->DCMRWF1 & DCM_GPR_DCMRWF1_RMII_MII_SEL_MASK) |
                           DCM_GPR_DCMRWF1_RMII_MII_SEL(0U);
    }
}

void ENET_QOS_EnableClock(bool enable)
{
    if (enable)
    {
        CLOCK_EnableClock(kCLOCK_Emac);
    }
    else
    {
        CLOCK_DisableClock(kCLOCK_Emac);
    }
}

static void MDIO_Init(void)
{
    ENET_QOS_SetSMI(EMAC, CLOCK_GetAipsPlatClkFreq());
}

static status_t MDIO_Write(uint8_t phyAddr, uint8_t regAddr, uint16_t data)
{
    return ENET_QOS_MDIOWrite(EMAC, phyAddr, regAddr, data);
}

static status_t MDIO_Read(uint8_t phyAddr, uint8_t regAddr, uint16_t *pData)
{
    return ENET_QOS_MDIORead(EMAC, phyAddr, regAddr, pData);
}

/*${function:start}*/
void BOARD_InitHardware(void)
{
    BOARD_ConfigMPU();
    BOARD_InitBootPins();
    BOARD_InitFlexCANPins();
    BOARD_InitEMACPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();
    BOARD_InitModuleClock();

    SIUL2_PortPinWrite(BOARD_INITEMACPINS_ENET_PHY_RST_SIUL2_BASE, BOARD_INITEMACPINS_ENET_PHY_RST_GPIO, 3U, 0U);
    SDK_DelayAtLeastUs(25000, CLOCK_GetFreq(kCLOCK_CoreSysClk));
    SIUL2_PortPinWrite(BOARD_INITEMACPINS_ENET_PHY_RST_SIUL2_BASE, BOARD_INITEMACPINS_ENET_PHY_RST_GPIO, 3U, 1U);

    g_phy_resource.read  = MDIO_Read;
    g_phy_resource.write = MDIO_Write;

    MDIO_Init();

    /*Clock setting for FLEXCAN*/
    CLOCK_SetClkDiv(kCLOCK_DivFlexcan012PeClk, 1U);
    CLOCK_AttachClk(kAIPS_PLAT_CLK_to_FLEXCAN012_PE);

    CLOCK_SetClkDiv(kCLOCK_DivFlexcan345PeClk, 1U);
    CLOCK_AttachClk(kAIPS_PLAT_CLK_to_FLEXCAN345_PE);
}
/*${function:end}*/
