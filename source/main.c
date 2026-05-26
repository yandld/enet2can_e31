/*
 * main.c - 6x FlexCAN Demo for MCXE31B (enet2can)
 *
 * Each CAN channel independently:
 *   - Transmits a counter frame every 1 s
 *   - Receives all standard data frames (except own TX) and logs them
 *   - Uses Classic CAN mode by default on all channels
 *
 * Pin assignment (see board/pin_mux.c):
 *   CAN0: FLEXCAN_0  TX=PTA7   RX=PTA6
 *   CAN1: FLEXCAN_1  TX=PTA11  RX=PTA12
 *   CAN2: FLEXCAN_2  TX=PTE24  RX=PTE25
 *   CAN3: FLEXCAN_3  TX=PTC28  RX=PTC29
 *   CAN4: FLEXCAN_4  TX=PTC30  RX=PTC31
 *   CAN5: FLEXCAN_5  TX=PTC27  RX=PTC26
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*******************************************************************************
 * Channel Configuration - Edit here to change CAN parameters
 ******************************************************************************/
#define CAN_CHANNEL_COUNT       6U
#define CAN_TX_ID_BASE          0x100U
#define CAN_BITRATE             500000U
#define CAN_USE_CANFD           0
#define CAN_FD_BITRATE          2000000U
#define TX_PERIOD_MS            1000U
#define TX_TIMEOUT_MS           200U
#define TX_MB_IDX               0
#define RX_MB_IDX               1
#define FD_PAYLOAD_SIZE         kFLEXCAN_64BperMB
#define FD_DLC                  15              /* DLC=15 means 64 bytes */

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include <string.h>
#include "fsl_debug_console.h"
#include "fsl_flexcan.h"
#include "fsl_clock.h"
#include "board.h"
#include "uart2.h"
#include "uart_smoke.h"

/*******************************************************************************
 * Types
 ******************************************************************************/
typedef struct
{
    CAN_Type    *base;
    clock_name_t clkName;
    uint32_t     txId;
} can_ch_config_t;

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
    uint32_t         txStartMs;

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
static can_ch_t          g_can[CAN_CHANNEL_COUNT];

static const can_ch_config_t s_canConfig[CAN_CHANNEL_COUNT] =
{
    { FLEXCAN_0, kCLOCK_Flexcan0Clk, CAN_TX_ID_BASE + 0U },
    { FLEXCAN_1, kCLOCK_Flexcan1Clk, CAN_TX_ID_BASE + 1U },
    { FLEXCAN_2, kCLOCK_Flexcan2Clk, CAN_TX_ID_BASE + 2U },
    { FLEXCAN_3, kCLOCK_Flexcan3Clk, CAN_TX_ID_BASE + 3U },
    { FLEXCAN_4, kCLOCK_Flexcan4Clk, CAN_TX_ID_BASE + 4U },
    { FLEXCAN_5, kCLOCK_Flexcan5Clk, CAN_TX_ID_BASE + 5U }
};

/*******************************************************************************
 * SysTick - 1 ms timebase
 ******************************************************************************/
void SysTick_Handler(void)
{
    g_ms++;
}

/*******************************************************************************
 * FlexCAN callback - shared by all channels, routed via userData
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

    /* Accept ALL standard data frames. Mask = 0 means no bits need to match. */
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
    status_t              status;

    if (!ch->txDone)
    {
        if ((g_ms - ch->txStartMs) >= TX_TIMEOUT_MS)
        {
            if (ch->useFD)
                FLEXCAN_TransferFDAbortSend(ch->base, &ch->handle, TX_MB_IDX);
            else
                FLEXCAN_TransferAbortSend(ch->base, &ch->handle, TX_MB_IDX);

            ch->txDone = true;
            PRINTF("[CAN%d] TX timeout, abort pending frame\r\n", ch->idx);
        }
        return;
    }

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
    ch->txStartMs = g_ms;

    if (ch->useFD)
        status = FLEXCAN_TransferFDSendNonBlocking(ch->base, &ch->handle, &xfer);
    else
        status = FLEXCAN_TransferSendNonBlocking(ch->base, &ch->handle, &xfer);

    if (status != kStatus_Success)
    {
        ch->txDone = true;
        PRINTF("[CAN%d] TX start failed, status=%d\r\n", ch->idx, (int)status);
        return;
    }

    ch->lastTxMs = g_ms;

    PRINTF("[CAN%d] TX start  id=0x%x  data=0x%x\r\n",
           ch->idx, ch->txId, ch->txCounter);
    ch->txCounter++;
}

/*******************************************************************************
 * Helper: CAN FD DLC to actual byte length
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
        PRINTF("%x ", (unsigned)((words[i >> 2] >> (24U - 8U * (i & 3U))) & 0xFFU));
}

/*******************************************************************************
 * Handle a received frame, log it, and re-arm RX
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

    PRINTF("[CAN%d] RX  id=0x%x  len=%u  data: ", ch->idx, rxId, rxLen);
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
    for (uint8_t i = 0U; i < CAN_CHANNEL_COUNT; i++)
    {
        g_can[i].base      = s_canConfig[i].base;
        g_can[i].clkFreq   = CLOCK_GetFreq(s_canConfig[i].clkName);
        g_can[i].bitRate   = CAN_BITRATE;
        g_can[i].useFD     = (bool)CAN_USE_CANFD;
        g_can[i].bitRateFD = CAN_FD_BITRATE;
        g_can[i].txId      = s_canConfig[i].txId;
        g_can[i].txDone    = true;
        g_can[i].idx       = i;
    }
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
    PRINTF("  6x FlexCAN Demo  -  MCXE31B\r\n");
    PRINTF("========================================\r\n");
    for (uint8_t i = 0U; i < CAN_CHANNEL_COUNT; i++)
    {
        can_ch_t *ch = &g_can[i];
        if (ch->useFD)
            PRINTF("  CAN%d: %ukbps (arb) / %ukbps (data)  CAN FD  TX=0x%x  period=%ums\r\n",
                   i,
                   (unsigned)(ch->bitRate   / 1000U),
                   (unsigned)(ch->bitRateFD / 1000U),
                   (unsigned)ch->txId,
                   TX_PERIOD_MS);
        else
            PRINTF("  CAN%d: %ukbps  Classic CAN  TX=0x%x  period=%ums\r\n",
                   i,
                   (unsigned)(ch->bitRate / 1000U),
                   (unsigned)ch->txId,
                   TX_PERIOD_MS);
    }
    PRINTF("  RX: all standard frames\r\n");
    PRINTF("========================================\r\n\r\n");

    for (uint8_t i = 0U; i < CAN_CHANNEL_COUNT; i++)
    {
        can_init(&g_can[i]);
        can_start_rx(&g_can[i]);
        PRINTF("[CAN%d] ready\r\n", i);
    }

    uart2_init();
    uart_smoke_init_all();
    PRINTF("\r\n");

    while (1)
    {
        for (uint8_t i = 0U; i < CAN_CHANNEL_COUNT; i++)
        {
            can_ch_t *ch = &g_can[i];

            if ((g_ms - ch->lastTxMs) >= TX_PERIOD_MS)
                can_send_periodic(ch);

            if (ch->rxDone)
                can_handle_rx(ch);
        }

        uart2_poll(g_ms);
        uart_smoke_poll_all(g_ms);
    }
}
