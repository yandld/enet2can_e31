/*
 * main.c — 3x FlexCAN Demo for MCXE31B (enet2can)
 *
 * Each CAN channel independently:
 *   - Transmits a counter frame every 1 s
 *   - Receives all standard data frames (except own TX) and logs them
 *   - CAN FD mode configurable per channel
 *
 * Pin assignment (see board/pin_mux.c):
 *   CAN0: FLEXCAN_0  TX=PTA7   RX=PTA6
 *   CAN1: FLEXCAN_1  TX=PTA11  RX=PTA12
 *   CAN2: FLEXCAN_2  TX=PTE24  RX=PTE25
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*******************************************************************************
 * Channel Configuration — Edit here to change CAN parameters
 ******************************************************************************/

/* CAN0 — FLEXCAN_0  TX=PTA7  RX=PTA6 */
#define CAN0_TX_ID          0x100U
#define CAN0_BITRATE        500000U
#define CAN0_USE_CANFD      0
#define CAN0_FD_BITRATE     2000000U

/* CAN1 — FLEXCAN_1  TX=PTA11  RX=PTA12 */
#define CAN1_TX_ID          0x101U
#define CAN1_BITRATE        500000U
#define CAN1_USE_CANFD      0
#define CAN1_FD_BITRATE     2000000U

/* CAN2 — FLEXCAN_2  TX=PTE24  RX=PTE25 */
#define CAN2_TX_ID          0x102U
#define CAN2_BITRATE        500000U
#define CAN2_USE_CANFD      0
#define CAN2_FD_BITRATE     2000000U

/* Common */
#define TX_PERIOD_MS        1000U               /* All channels: 1 s TX interval */
#define TX_MB_IDX           0
#define RX_MB_IDX           1
#define FD_PAYLOAD_SIZE     kFLEXCAN_64BperMB
#define FD_DLC              15                  /* DLC=15 → 64 bytes */

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include <string.h>
#include "fsl_debug_console.h"
#include "fsl_flexcan.h"
#include "board.h"

/*******************************************************************************
 * Types
 ******************************************************************************/
typedef struct
{
    CAN_Type *base;
    uint32_t  clkFreq;
    uint32_t  bitRate;
    bool      useFD;
    uint32_t  bitRateFD;
    uint32_t  txId;
    uint8_t   idx;

    flexcan_handle_t handle;
    volatile bool    txDone;
    volatile bool    rxDone;
    uint32_t         txCounter;
    uint32_t         lastTxMs;

    union
    {
        flexcan_frame_t    classic;
        flexcan_fd_frame_t fd;
    } txFrame, rxFrame;
} can_ch_t;

/*******************************************************************************
 * Globals
 ******************************************************************************/
static volatile uint32_t g_ms;
static can_ch_t          g_can[3];

/*******************************************************************************
 * SysTick — 1 ms timebase
 ******************************************************************************/
void SysTick_Handler(void)
{
    g_ms++;
}

/*******************************************************************************
 * FlexCAN callback — shared by all 3 channels, routed via userData
 ******************************************************************************/
static FLEXCAN_CALLBACK(can_callback)
{
    can_ch_t *ch = (can_ch_t *)userData;
    switch (status)
    {
        case kStatus_FLEXCAN_RxIdle:
            if (result == RX_MB_IDX) ch->rxDone = true;
            break;
        case kStatus_FLEXCAN_TxIdle:
            if (result == TX_MB_IDX) ch->txDone = true;
            break;
        default:
            break;
    }
}

/*******************************************************************************
 * Initialise one CAN channel
 ******************************************************************************/
static void can_init(can_ch_t *ch)
{
    flexcan_config_t        cfg;
    flexcan_rx_mb_config_t  mbRxCfg;
    flexcan_timing_config_t timing;

    FLEXCAN_GetDefaultConfig(&cfg);
    cfg.bitRate              = ch->bitRate;
    cfg.disableSelfReception = true; /* Do not receive own TX frames */

    if (ch->useFD)
    {
        cfg.bitRateFD = ch->bitRateFD;
        memset(&timing, 0, sizeof(timing));
        if (!FLEXCAN_FDCalculateImprovedTimingValues(ch->base, cfg.bitRate,
                cfg.bitRateFD, ch->clkFreq, &timing))
            PRINTF("[CAN%d] Warning: FD timing calc failed\r\n", ch->idx);
        else
            memcpy(&cfg.timingConfig, &timing, sizeof(timing));
        FLEXCAN_FDInit(ch->base, &cfg, ch->clkFreq, FD_PAYLOAD_SIZE, true);
    }
    else
    {
        memset(&timing, 0, sizeof(timing));
        if (!FLEXCAN_CalculateImprovedTimingValues(ch->base, cfg.bitRate,
                ch->clkFreq, &timing))
            PRINTF("[CAN%d] Warning: timing calc failed\r\n", ch->idx);
        else
            memcpy(&cfg.timingConfig, &timing, sizeof(timing));
        FLEXCAN_Init(ch->base, &cfg, ch->clkFreq);
    }

    FLEXCAN_TransferCreateHandle(ch->base, &ch->handle, can_callback, ch);

    /* Accept ALL standard data frames — mask = 0 means no bits need to match */
    FLEXCAN_SetRxMbGlobalMask(ch->base, 0U);

    mbRxCfg.format = kFLEXCAN_FrameFormatStandard;
    mbRxCfg.type   = kFLEXCAN_FrameTypeData;
    mbRxCfg.id     = FLEXCAN_ID_STD(0U);

    if (ch->useFD)
    {
        FLEXCAN_SetFDRxMbConfig(ch->base, RX_MB_IDX, &mbRxCfg, true);
        FLEXCAN_SetFDTxMbConfig(ch->base, TX_MB_IDX, true);
    }
    else
    {
        FLEXCAN_SetRxMbConfig(ch->base, RX_MB_IDX, &mbRxCfg, true);
        FLEXCAN_SetTxMbConfig(ch->base, TX_MB_IDX, true);
    }
}

/*******************************************************************************
 * Arm the RX message buffer (call once after init, then after each received frame)
 ******************************************************************************/
static void can_start_rx(can_ch_t *ch)
{
    flexcan_mb_transfer_t xfer;
    ch->rxDone = false;
    xfer.mbIdx = RX_MB_IDX;
    if (ch->useFD)
    {
        xfer.framefd = &ch->rxFrame.fd;
        (void)FLEXCAN_TransferFDReceiveNonBlocking(ch->base, &ch->handle, &xfer);
    }
    else
    {
        xfer.frame = &ch->rxFrame.classic;
        (void)FLEXCAN_TransferReceiveNonBlocking(ch->base, &ch->handle, &xfer);
    }
}

/*******************************************************************************
 * Send a periodic counter frame
 ******************************************************************************/
static void can_send_periodic(can_ch_t *ch)
{
    flexcan_mb_transfer_t xfer;

    ch->lastTxMs = g_ms;

    if (!ch->txDone)
        return; /* Previous TX still pending */

    memset(&ch->txFrame, 0, sizeof(ch->txFrame));

    if (ch->useFD)
    {
        ch->txFrame.fd.id          = FLEXCAN_ID_STD(ch->txId);
        ch->txFrame.fd.format      = (uint8_t)kFLEXCAN_FrameFormatStandard;
        ch->txFrame.fd.type        = (uint8_t)kFLEXCAN_FrameTypeData;
        ch->txFrame.fd.length      = (uint8_t)FD_DLC;
        ch->txFrame.fd.brs         = 1U;
        ch->txFrame.fd.edl         = 1U;
        ch->txFrame.fd.dataWord[0] = ch->txCounter;
        xfer.mbIdx   = TX_MB_IDX;
        xfer.framefd = &ch->txFrame.fd;
    }
    else
    {
        ch->txFrame.classic.id        = FLEXCAN_ID_STD(ch->txId);
        ch->txFrame.classic.format    = (uint8_t)kFLEXCAN_FrameFormatStandard;
        ch->txFrame.classic.type      = (uint8_t)kFLEXCAN_FrameTypeData;
        ch->txFrame.classic.length    = 8U;
        ch->txFrame.classic.dataWord0 = ch->txCounter;
        xfer.mbIdx = TX_MB_IDX;
        xfer.frame = &ch->txFrame.classic;
    }

    ch->txDone = false;

    if (ch->useFD)
        (void)FLEXCAN_TransferFDSendNonBlocking(ch->base, &ch->handle, &xfer);
    else
        (void)FLEXCAN_TransferSendNonBlocking(ch->base, &ch->handle, &xfer);

    PRINTF("[CAN%d] TX  id=0x%03x  data=0x%08x\r\n",
           ch->idx, ch->txId, ch->txCounter);
    ch->txCounter++;
}

/*******************************************************************************
 * Helper: CAN FD DLC → actual byte length
 ******************************************************************************/
static uint8_t dlc_to_len(uint8_t dlc)
{
    static const uint8_t tbl[16] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
    return (dlc < 16U) ? tbl[dlc] : 64U;
}

/*******************************************************************************
 * Helper: print frame payload as hex bytes
 * FlexCAN stores bytes big-endian within each 32-bit word:
 *   word[n] bits 31-24 = byte[4n+0], bits 23-16 = byte[4n+1], ...
 ******************************************************************************/
static void print_frame_bytes(const uint32_t *words, uint8_t byteLen)
{
    for (uint8_t i = 0; i < byteLen; i++)
        PRINTF("%02X ", (unsigned)((words[i >> 2] >> (24U - 8U * (i & 3U))) & 0xFFU));
}

/*******************************************************************************
 * Handle a received frame — log it and re-arm RX
 ******************************************************************************/
static void can_handle_rx(can_ch_t *ch)
{
    uint32_t        rxId;
    uint8_t         rxLen;
    const uint32_t *rxWords;
    uint32_t        rxTs;

    if (ch->useFD)
    {
        rxId    = ch->rxFrame.fd.id >> CAN_ID_STD_SHIFT;
        rxLen   = dlc_to_len(ch->rxFrame.fd.length);
        rxWords = ch->rxFrame.fd.dataWord;
        rxTs    = ch->rxFrame.fd.timestamp;
    }
    else
    {
        rxId    = ch->rxFrame.classic.id >> CAN_ID_STD_SHIFT;
        rxLen   = ch->rxFrame.classic.length;
        rxWords = &ch->rxFrame.classic.dataWord0;
        rxTs    = ch->rxFrame.classic.timestamp;
    }

    PRINTF("[CAN%d] RX  id=0x%03x  len=%-2u  data: ", ch->idx, rxId, rxLen);
    print_frame_bytes(rxWords, rxLen);
    PRINTF(" ts=%u\r\n", rxTs);

    ch->rxDone = false;
    can_start_rx(ch);
}

/*******************************************************************************
 * Runtime channel setup
 ******************************************************************************/
static void can_setup(void)
{
    g_can[0].base    = FLEXCAN_0;
    g_can[0].clkFreq = CLOCK_GetFreq(kCLOCK_Flexcan0Clk);
    g_can[0].bitRate = CAN0_BITRATE;
    g_can[0].useFD   = (bool)CAN0_USE_CANFD;
    g_can[0].bitRateFD = CAN0_FD_BITRATE;
    g_can[0].txId    = CAN0_TX_ID;
    g_can[0].txDone  = true;
    g_can[0].idx     = 0U;

    g_can[1].base    = FLEXCAN_1;
    g_can[1].clkFreq = CLOCK_GetFreq(kCLOCK_Flexcan1Clk);
    g_can[1].bitRate = CAN1_BITRATE;
    g_can[1].useFD   = (bool)CAN1_USE_CANFD;
    g_can[1].bitRateFD = CAN1_FD_BITRATE;
    g_can[1].txId    = CAN1_TX_ID;
    g_can[1].txDone  = true;
    g_can[1].idx     = 1U;

    g_can[2].base    = FLEXCAN_2;
    g_can[2].clkFreq = CLOCK_GetFreq(kCLOCK_Flexcan2Clk);
    g_can[2].bitRate = CAN2_BITRATE;
    g_can[2].useFD   = (bool)CAN2_USE_CANFD;
    g_can[2].bitRateFD = CAN2_FD_BITRATE;
    g_can[2].txId    = CAN2_TX_ID;
    g_can[2].txDone  = true;
    g_can[2].idx     = 2U;
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void)
{
    BOARD_InitHardware();
    SysTick_Config(SystemCoreClock / 1000U);

    can_setup(); /* must run before reading g_can[] for startup print */

    PRINTF("\r\n========================================\r\n");
    PRINTF("  3x FlexCAN Demo  —  MCXE31B\r\n");
    PRINTF("========================================\r\n");
    for (int i = 0; i < 3; i++)
    {
        can_ch_t *ch = &g_can[i];
        if (ch->useFD)
            PRINTF("  CAN%d: %lukbps (arb) / %lukbps (data)  CAN FD  TX=0x%03x  period=%ums\r\n",
                   i,
                   (unsigned long)(ch->bitRate   / 1000U),
                   (unsigned long)(ch->bitRateFD / 1000U),
                   (unsigned)ch->txId,
                   TX_PERIOD_MS);
        else
            PRINTF("  CAN%d: %lukbps  Classic CAN  TX=0x%03x  period=%ums\r\n",
                   i,
                   (unsigned long)(ch->bitRate / 1000U),
                   (unsigned)ch->txId,
                   TX_PERIOD_MS);
    }
    PRINTF("  RX: all standard frames\r\n");
    PRINTF("========================================\r\n\r\n");

    for (int i = 0; i < 3; i++)
    {
        can_init(&g_can[i]);
        can_start_rx(&g_can[i]);
        PRINTF("[CAN%d] ready\r\n", i);
    }
    PRINTF("\r\n");

    while (1)
    {
        for (int i = 0; i < 3; i++)
        {
            can_ch_t *ch = &g_can[i];

            if ((g_ms - ch->lastTxMs) >= TX_PERIOD_MS)
                can_send_periodic(ch);

            if (ch->rxDone)
                can_handle_rx(ch);
        }
    }
}
