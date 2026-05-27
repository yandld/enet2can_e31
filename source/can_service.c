/*
 * can_service.c - CAN service boundary for the CAN0-first gateway slice
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "can_service.h"

#include <string.h>

#include "fsl_clock.h"
#include "fsl_debug_console.h"
#include "fsl_flexcan.h"

#define CAN_TX_ID_BASE 0x100U
#define TX_PERIOD_MS 1000U
#define TX_TIMEOUT_MS 200U
#define STATUS_PERIOD_MS 5000U
#define TX_MB_IDX 0
#define RX_MB_IDX 1
#define FD_PAYLOAD_SIZE kFLEXCAN_64BperMB
#define CAN_SERVICE_ENABLE_PERIODIC_TX 0U
#define CAN_ERROR_WARNING_THRESHOLD 96U
#define CAN_MIN_NOMINAL_BITRATE 50000U
#define CAN_MAX_NOMINAL_BITRATE 1000000U
#define CAN_MIN_DATA_BITRATE 500000U
#define CAN_MAX_DATA_BITRATE 5000000U

typedef struct
{
    CAN_Type *base;
    clock_name_t clkName;
    uint32_t txId;
} can_channel_config_t;

typedef struct
{
    CAN_Type *base;
    uint32_t clkFreq;
    uint32_t bitRate;
    bool useFD;
    uint32_t bitRateFD;
    uint32_t txId;
    uint8_t index;
    can_service_config_t config;

    flexcan_handle_t handle;
    volatile bool txDone;
    volatile bool rxDone;
    volatile bool rxFifoDone;
    volatile bool rxFifoRestartNeeded;
    uint32_t txCounter;
    uint32_t lastTxMs;
    uint32_t txStartMs;

    can_service_status_t status;
    can_gateway_frame_t rxRing[CAN_SERVICE_RX_RING_SIZE];
    uint8_t rxHead;
    uint8_t rxTail;
    uint8_t rxQueued;
    can_gateway_frame_t txQueue[CAN_SERVICE_TX_QUEUE_SIZE];
    uint8_t txHead;
    uint8_t txTail;
    uint8_t txQueued;

    union
    {
        flexcan_frame_t classic;
        flexcan_fd_frame_t fd;
    } txFrame, rxFrame;
    flexcan_fifo_transfer_t rxFifoXfer;
    flexcan_fd_frame_t rxFifoFrame[CAN_SERVICE_EFIFO_BATCH_SIZE];
} can_channel_t;

static volatile uint32_t s_ms;
static uint32_t s_lastStatusMs;
static bool s_initialized;
static uint32_t s_activeMask;
static uint32_t s_lastConfigStatus = CAN_SERVICE_CONFIG_OK;
static can_channel_t s_can[CAN_SERVICE_CHANNEL_COUNT];

static const can_channel_config_t s_canConfig[CAN_SERVICE_CHANNEL_COUNT] =
{
    { FLEXCAN_0, kCLOCK_Flexcan0Clk, CAN_TX_ID_BASE + 0U },
    { FLEXCAN_1, kCLOCK_Flexcan1Clk, CAN_TX_ID_BASE + 1U },
    { FLEXCAN_2, kCLOCK_Flexcan2Clk, CAN_TX_ID_BASE + 2U },
    { FLEXCAN_3, kCLOCK_Flexcan3Clk, CAN_TX_ID_BASE + 3U },
    { FLEXCAN_4, kCLOCK_Flexcan4Clk, CAN_TX_ID_BASE + 4U },
    { FLEXCAN_5, kCLOCK_Flexcan5Clk, CAN_TX_ID_BASE + 5U }
};

static uint32_t s_enhancedRxFifoFilterTable[] =
{
    FLEXCAN_ENHANCED_RX_FIFO_EXT_MASK_AND_FILTER_LOW(0U, 0U),
    FLEXCAN_ENHANCED_RX_FIFO_EXT_MASK_AND_FILTER_HIGH(0U, 0U),
    FLEXCAN_ENHANCED_RX_FIFO_STD_MASK_AND_FILTER(0U, 0U, 0U, 0U),
    FLEXCAN_ENHANCED_RX_FIFO_STD_MASK_AND_FILTER(0U, 0U, 0U, 0U)
};

static void can_init_channel(can_channel_t *ch);

static uint8_t dlc_to_len(uint8_t dlc)
{
    return (dlc <= CAN_FD_DLC) ? (uint8_t)DLC_LENGTH_DECODE(dlc) : CAN_GATEWAY_MAX_DATA_LEN;
}

static void copy_words_to_bytes(uint8_t *bytes, const uint32_t *words, uint8_t byteLen)
{
    for (uint8_t i = 0U; i < byteLen; i++)
    {
        bytes[i] = (uint8_t)((words[i >> 2] >> (24U - (8U * (i & 3U)))) & 0xFFU);
    }
}

static void copy_bytes_to_words(uint32_t *words, const uint8_t *bytes, uint8_t byteLen)
{
    uint8_t clearLen = (uint8_t)((byteLen + 3U) & ~3U);

    if (clearLen != 0U)
    {
        memset(words, 0, clearLen);
    }

    for (uint8_t i = 0U; i < byteLen; i++)
    {
        words[i >> 2] |= ((uint32_t)bytes[i] << (24U - (8U * (i & 3U))));
    }
}

static bool can_channel_enabled(uint8_t channel)
{
    return (channel < CAN_SERVICE_CHANNEL_COUNT) && ((s_activeMask & (1UL << channel)) != 0U);
}

static can_service_config_t can_default_config(bool enabled)
{
    can_service_config_t config;

    memset(&config, 0, sizeof(config));
    config.enabled = enabled;
    config.useFD = (bool)CAN_USE_CANFD;
    config.brs = true;
    config.bitRate = CAN_BITRATE;
    config.bitRateFD = CAN_FD_BITRATE;
    config.filterMode = CAN_SERVICE_FILTER_ACCEPT_ALL;
    config.filterId = 0U;
    config.filterMask = 0U;
    config.txDropPolicy = CAN_SERVICE_TX_DROP_NEWEST;

    return config;
}

static void can_reset_queues(can_channel_t *ch)
{
    ch->rxHead = 0U;
    ch->rxTail = 0U;
    ch->rxQueued = 0U;
    ch->txHead = 0U;
    ch->txTail = 0U;
    ch->txQueued = 0U;
    ch->status.rxQueued = 0U;
    ch->status.txQueued = 0U;
    ch->rxDone = false;
    ch->rxFifoDone = false;
    ch->rxFifoRestartNeeded = false;
    ch->txDone = true;
}

static void can_sync_channel_from_config(can_channel_t *ch)
{
    ch->bitRate = ch->config.bitRate;
    ch->useFD = ch->config.useFD;
    ch->bitRateFD = ch->config.useFD ? ch->config.bitRateFD : 0U;
    ch->status.enabled = ch->config.enabled;
    ch->status.useFD = ch->config.useFD;
    ch->status.bitRate = ch->config.bitRate;
    ch->status.bitRateFD = ch->bitRateFD;
}

static uint32_t can_validate_config(uint8_t channel, const can_service_config_t *config)
{
    if (channel != 0U)
    {
        return CAN_SERVICE_CONFIG_UNSUPPORTED_CHANNEL;
    }

    if (config == NULL)
    {
        return CAN_SERVICE_CONFIG_NULL;
    }

    if ((config->bitRate < CAN_MIN_NOMINAL_BITRATE) || (config->bitRate > CAN_MAX_NOMINAL_BITRATE))
    {
        return CAN_SERVICE_CONFIG_INVALID_BITRATE;
    }

    if (config->useFD &&
        ((config->bitRateFD < CAN_MIN_DATA_BITRATE) || (config->bitRateFD > CAN_MAX_DATA_BITRATE)))
    {
        return CAN_SERVICE_CONFIG_INVALID_BITRATE;
    }

    if ((config->filterMode != CAN_SERVICE_FILTER_ACCEPT_ALL) &&
        (config->filterMode != CAN_SERVICE_FILTER_ID_MASK))
    {
        return CAN_SERVICE_CONFIG_INVALID_MODE;
    }

    if ((config->txDropPolicy != CAN_SERVICE_TX_DROP_NEWEST) &&
        (config->txDropPolicy != CAN_SERVICE_TX_DROP_OLDEST))
    {
        return CAN_SERVICE_CONFIG_INVALID_MODE;
    }

    if ((config->filterId > 0x1FFFFFFFUL) || (config->filterMask > 0x1FFFFFFFUL))
    {
        return CAN_SERVICE_CONFIG_INVALID_FILTER;
    }

    return CAN_SERVICE_CONFIG_OK;
}

static void can_deinit_channel(can_channel_t *ch)
{
    if (!ch->status.enabled)
    {
        return;
    }

#if CAN_SERVICE_USE_ENHANCED_RX_FIFO
    if (ch->useFD)
    {
        FLEXCAN_TransferAbortReceiveEnhancedFifo(ch->base, &ch->handle);
    }
    else
#endif
    {
        FLEXCAN_TransferAbortReceive(ch->base, &ch->handle, RX_MB_IDX);
    }

    if (ch->useFD)
    {
        FLEXCAN_TransferFDAbortSend(ch->base, &ch->handle, TX_MB_IDX);
    }
    else
    {
        FLEXCAN_TransferAbortSend(ch->base, &ch->handle, TX_MB_IDX);
    }

    FLEXCAN_Deinit(ch->base);
}

static uint32_t can_apply_config(uint8_t channel, const can_service_config_t *config)
{
    can_channel_t *ch = &s_can[channel];

    if (s_initialized)
    {
        can_deinit_channel(ch);
    }

    ch->config = *config;
    if (!ch->config.useFD)
    {
        ch->config.bitRateFD = 0U;
        ch->config.brs = false;
    }

    if (ch->config.enabled)
    {
        s_activeMask |= (1UL << channel);
    }
    else
    {
        s_activeMask &= ~(1UL << channel);
    }

    can_sync_channel_from_config(ch);
    can_reset_queues(ch);

    if (s_initialized && ch->config.enabled)
    {
        can_init_channel(ch);
    }

    return CAN_SERVICE_CONFIG_OK;
}

static bool can_frame_matches_filter(const can_channel_t *ch, const can_gateway_frame_t *frame)
{
    if (ch->config.filterMode == CAN_SERVICE_FILTER_ACCEPT_ALL)
    {
        return true;
    }

    return ((frame->can_id & ch->config.filterMask) == (ch->config.filterId & ch->config.filterMask));
}

static void can_start_rx(can_channel_t *ch)
{
    flexcan_mb_transfer_t xfer;
    status_t status;

    ch->rxDone = false;

#if CAN_SERVICE_USE_ENHANCED_RX_FIFO
    if (ch->useFD)
    {
        ch->rxFifoDone = false;
        ch->rxFifoXfer.framefd = &ch->rxFifoFrame[0];
        ch->rxFifoXfer.frameNum = CAN_SERVICE_EFIFO_BATCH_SIZE;
        status = FLEXCAN_TransferReceiveEnhancedFifoNonBlocking(ch->base, &ch->handle, &ch->rxFifoXfer);
        if ((status != kStatus_Success) && (status != kStatus_FLEXCAN_RxFifoBusy))
        {
            ch->status.rxErrorCount++;
            ch->status.lastErrorStatus = (uint32_t)status;
        }
        return;
    }
#endif

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

static FLEXCAN_CALLBACK(can_callback)
{
    can_channel_t *ch = (can_channel_t *)userData;

    switch (status)
    {
        case kStatus_FLEXCAN_RxIdle:
            if (result == RX_MB_IDX)
            {
                ch->rxDone = true;
            }
            break;

        case kStatus_FLEXCAN_RxFifoIdle:
            ch->rxFifoDone = true;
            ch->status.rxFifoIdleCount++;
            break;

        case kStatus_FLEXCAN_TxIdle:
            if (result == TX_MB_IDX)
            {
                ch->txDone = true;
                ch->status.txDoneCount++;
            }
            break;

        case kStatus_FLEXCAN_RxOverflow:
            ch->status.rxErrorCount++;
            ch->status.lastErrorStatus = (uint32_t)result;
            ch->rxDone = true;
            break;

        case kStatus_FLEXCAN_RxFifoOverflow:
            ch->status.rxFifoOverflowCount++;
            ch->status.rxErrorCount++;
            ch->status.lastErrorStatus = (uint32_t)result;
            ch->rxFifoRestartNeeded = true;
            break;

        case kStatus_FLEXCAN_RxFifoWarning:
            ch->status.rxFifoWarningCount++;
            ch->status.lastErrorStatus = (uint32_t)result;
            break;

        case kStatus_FLEXCAN_RxFifoUnderflow:
            ch->status.rxFifoUnderflowCount++;
            ch->status.rxErrorCount++;
            ch->status.lastErrorStatus = (uint32_t)result;
            ch->rxFifoRestartNeeded = true;
            break;

        case kStatus_FLEXCAN_ErrorStatus:
            if (((uint32_t)result & (uint32_t)kFLEXCAN_BusOffIntFlag) != 0U)
            {
                ch->status.busOffCount++;
            }
            ch->status.txErrorCount++;
            ch->status.lastErrorStatus = (uint32_t)result;
            break;

        case kStatus_FLEXCAN_MemoryError:
        case kStatus_FLEXCAN_UnHandled:
            ch->status.txErrorCount++;
            ch->status.lastErrorStatus = (uint32_t)result;
            break;

        default:
            break;
    }
}

static void can_init_channel(can_channel_t *ch)
{
    flexcan_config_t cfg;
    flexcan_rx_mb_config_t mbRxCfg;
#if CAN_SERVICE_USE_ENHANCED_RX_FIFO
    flexcan_enhanced_rx_fifo_config_t fifoCfg;
#endif
    flexcan_timing_config_t timing;

    FLEXCAN_GetDefaultConfig(&cfg);
    cfg.bitRate = ch->bitRate;
    cfg.disableSelfReception = true;

    if (ch->useFD)
    {
        cfg.bitRateFD = ch->bitRateFD;
        memset(&timing, 0, sizeof(timing));

        if (FLEXCAN_FDCalculateImprovedTimingValues(ch->base, cfg.bitRate, cfg.bitRateFD, ch->clkFreq, &timing))
        {
            memcpy(&cfg.timingConfig, &timing, sizeof(timing));
        }
        else
        {
            PRINTF("CAN%d: warning FD timing calculation failed\r\n", ch->index);
        }

        FLEXCAN_FDInit(ch->base, &cfg, ch->clkFreq, FD_PAYLOAD_SIZE, true);
    }
    else
    {
        memset(&timing, 0, sizeof(timing));

        if (FLEXCAN_CalculateImprovedTimingValues(ch->base, cfg.bitRate, ch->clkFreq, &timing))
        {
            memcpy(&cfg.timingConfig, &timing, sizeof(timing));
        }
        else
        {
            PRINTF("CAN%d: warning timing calculation failed\r\n", ch->index);
        }

        FLEXCAN_Init(ch->base, &cfg, ch->clkFreq);
    }

    FLEXCAN_TransferCreateHandle(ch->base, &ch->handle, can_callback, ch);
    FLEXCAN_SetRxMbGlobalMask(ch->base, 0U);

    mbRxCfg.format = kFLEXCAN_FrameFormatStandard;
    mbRxCfg.type = kFLEXCAN_FrameTypeData;
    mbRxCfg.id = FLEXCAN_ID_STD(0U);

    if (ch->useFD)
    {
        FLEXCAN_SetFDTxMbConfig(ch->base, TX_MB_IDX, true);

#if CAN_SERVICE_USE_ENHANCED_RX_FIFO
        memset(&fifoCfg, 0, sizeof(fifoCfg));
        fifoCfg.idFilterTable = s_enhancedRxFifoFilterTable;
        fifoCfg.idFilterPairNum = 2U;
        fifoCfg.extendIdFilterNum = 1U;
        fifoCfg.fifoWatermark = CAN_SERVICE_EFIFO_WATERMARK;
        fifoCfg.dmaPerReadLength = kFLEXCAN_19WordPerRead;
        fifoCfg.priority = kFLEXCAN_RxFifoPrioHigh;
        FLEXCAN_SetEnhancedRxFifoConfig(ch->base, &fifoCfg, true);
#endif
    }
    else
    {
        FLEXCAN_SetRxMbConfig(ch->base, RX_MB_IDX, &mbRxCfg, true);
        FLEXCAN_SetTxMbConfig(ch->base, TX_MB_IDX, true);
    }

    can_start_rx(ch);
}

static bool can_validate_tx_frame(const can_channel_t *ch, const can_gateway_frame_t *frame)
{
    uint8_t dataLen = dlc_to_len(frame->dlc);
    bool isExtended = ((frame->flags & CAN_GATEWAY_FLAG_EXTENDED_ID) != 0U);
    bool isFD = ((frame->flags & CAN_GATEWAY_FLAG_FD) != 0U);
    uint8_t knownFlags = CAN_GATEWAY_FLAG_FD | CAN_GATEWAY_FLAG_BRS | CAN_GATEWAY_FLAG_EXTENDED_ID |
                         CAN_GATEWAY_FLAG_REMOTE | CAN_GATEWAY_FLAG_ERROR;

    if ((frame->magic != CAN_GATEWAY_MAGIC) || (frame->version != CAN_GATEWAY_VERSION) ||
        (frame->channel >= CAN_SERVICE_CHANNEL_COUNT) || (frame->dlc > CAN_FD_DLC) ||
        ((frame->flags & CAN_GATEWAY_FLAG_ERROR) != 0U) || ((frame->flags & (uint8_t)~knownFlags) != 0U))
    {
        return false;
    }

    if ((!isExtended && (frame->can_id > 0x7FFU)) || (isExtended && (frame->can_id > 0x1FFFFFFFUL)))
    {
        return false;
    }

    if (!ch->useFD && isFD)
    {
        return false;
    }

    if (!isFD && (dataLen > CAN_CLASSIC_DLC))
    {
        return false;
    }

    if (isFD && ((frame->flags & CAN_GATEWAY_FLAG_REMOTE) != 0U))
    {
        return false;
    }

    if (((frame->flags & CAN_GATEWAY_FLAG_BRS) != 0U) && !ch->config.brs)
    {
        return false;
    }

    return true;
}

static bool can_prepare_tx_frame(can_channel_t *ch, const can_gateway_frame_t *frame, flexcan_mb_transfer_t *xfer)
{
    uint8_t dataLen = dlc_to_len(frame->dlc);
    bool isExtended = ((frame->flags & CAN_GATEWAY_FLAG_EXTENDED_ID) != 0U);
    bool isFD = ((frame->flags & CAN_GATEWAY_FLAG_FD) != 0U);

    if (!can_validate_tx_frame(ch, frame))
    {
        return false;
    }

    memset(&ch->txFrame, 0, sizeof(ch->txFrame));

    xfer->mbIdx = TX_MB_IDX;

    if (ch->useFD)
    {
        ch->txFrame.fd.id = isExtended ? FLEXCAN_ID_EXT(frame->can_id) : FLEXCAN_ID_STD(frame->can_id);
        ch->txFrame.fd.format = isExtended ? (uint8_t)kFLEXCAN_FrameFormatExtend : (uint8_t)kFLEXCAN_FrameFormatStandard;
        ch->txFrame.fd.type = ((frame->flags & CAN_GATEWAY_FLAG_REMOTE) != 0U) ?
                              (uint8_t)kFLEXCAN_FrameTypeRemote : (uint8_t)kFLEXCAN_FrameTypeData;
        ch->txFrame.fd.length = frame->dlc;
        ch->txFrame.fd.brs = (((frame->flags & CAN_GATEWAY_FLAG_BRS) != 0U) && ch->config.brs) ? 1U : 0U;
        ch->txFrame.fd.edl = isFD ? 1U : 0U;
        copy_bytes_to_words(ch->txFrame.fd.dataWord, frame->data, dataLen);
        xfer->framefd = &ch->txFrame.fd;
    }
    else
    {
        ch->txFrame.classic.id = isExtended ? FLEXCAN_ID_EXT(frame->can_id) : FLEXCAN_ID_STD(frame->can_id);
        ch->txFrame.classic.format = isExtended ? (uint8_t)kFLEXCAN_FrameFormatExtend : (uint8_t)kFLEXCAN_FrameFormatStandard;
        ch->txFrame.classic.type = ((frame->flags & CAN_GATEWAY_FLAG_REMOTE) != 0U) ?
                                   (uint8_t)kFLEXCAN_FrameTypeRemote : (uint8_t)kFLEXCAN_FrameTypeData;
        ch->txFrame.classic.length = dataLen;
        copy_bytes_to_words(&ch->txFrame.classic.dataWord0, frame->data, dataLen);
        xfer->frame = &ch->txFrame.classic;
    }

    return true;
}

static uint32_t can_start_tx(can_channel_t *ch, const can_gateway_frame_t *frame)
{
    flexcan_mb_transfer_t xfer;
    status_t status;

    if (!ch->txDone)
    {
        ch->status.txBusyCount++;
        return CAN_GATEWAY_STATUS_CAN_TX_BUSY;
    }

    if (!can_prepare_tx_frame(ch, frame, &xfer))
    {
        ch->status.txErrorCount++;
        return CAN_GATEWAY_STATUS_INVALID_PACKET;
    }

    ch->txDone = false;
    ch->txStartMs = s_ms;

    if (ch->useFD)
    {
        status = FLEXCAN_TransferFDSendNonBlocking(ch->base, &ch->handle, &xfer);
    }
    else
    {
        status = FLEXCAN_TransferSendNonBlocking(ch->base, &ch->handle, &xfer);
    }

    if (status != kStatus_Success)
    {
        ch->txDone = true;
        ch->status.txErrorCount++;
        ch->status.lastErrorStatus = (uint32_t)status;
        PRINTF("CAN%d: TX start failed status=%d\r\n", ch->index, (int)status);
        return CAN_GATEWAY_STATUS_CAN_TX_ERROR;
    }

    ch->lastTxMs = s_ms;
    ch->status.txStartCount++;
    return CAN_GATEWAY_STATUS_OK;
}

static uint32_t can_enqueue_tx(can_channel_t *ch, const can_gateway_frame_t *frame)
{
    if (!can_validate_tx_frame(ch, frame))
    {
        ch->status.txErrorCount++;
        return CAN_GATEWAY_STATUS_INVALID_PACKET;
    }

    if (ch->txQueued >= CAN_SERVICE_TX_QUEUE_SIZE)
    {
        ch->status.txDropCount++;
        if (ch->config.txDropPolicy == CAN_SERVICE_TX_DROP_NEWEST)
        {
            return CAN_GATEWAY_STATUS_QUEUE_FULL;
        }

        ch->txTail = (uint8_t)((ch->txTail + 1U) % CAN_SERVICE_TX_QUEUE_SIZE);
        ch->txQueued--;
    }

    ch->txQueue[ch->txHead] = *frame;
    ch->txHead = (uint8_t)((ch->txHead + 1U) % CAN_SERVICE_TX_QUEUE_SIZE);
    ch->txQueued++;
    ch->status.txQueued = ch->txQueued;

    if (ch->status.txQueued > ch->status.txQueueWatermark)
    {
        ch->status.txQueueWatermark = ch->status.txQueued;
    }

    return CAN_GATEWAY_STATUS_OK;
}

static void can_pop_tx(can_channel_t *ch)
{
    if (ch->txQueued == 0U)
    {
        return;
    }

    ch->txTail = (uint8_t)((ch->txTail + 1U) % CAN_SERVICE_TX_QUEUE_SIZE);
    ch->txQueued--;
    ch->status.txQueued = ch->txQueued;
}

static void can_process_tx_queue(can_channel_t *ch)
{
    uint32_t status;

    if (!ch->txDone || (ch->txQueued == 0U))
    {
        return;
    }

    status = can_start_tx(ch, &ch->txQueue[ch->txTail]);
    if (status == CAN_GATEWAY_STATUS_OK)
    {
        can_pop_tx(ch);
    }
    else if (status != CAN_GATEWAY_STATUS_CAN_TX_BUSY)
    {
        ch->status.txDropCount++;
        can_pop_tx(ch);
    }
}

static void can_abort_timed_out_tx(can_channel_t *ch)
{
    if (ch->txDone || ((s_ms - ch->txStartMs) < TX_TIMEOUT_MS))
    {
        return;
    }

    if (ch->useFD)
    {
        FLEXCAN_TransferFDAbortSend(ch->base, &ch->handle, TX_MB_IDX);
    }
    else
    {
        FLEXCAN_TransferAbortSend(ch->base, &ch->handle, TX_MB_IDX);
    }

    ch->txDone = true;
    ch->status.txTimeoutCount++;
    ch->status.txErrorCount++;
    PRINTF("CAN%d: TX timeout, abort pending frame\r\n", ch->index);
}

static void can_send_periodic(can_channel_t *ch)
{
    can_gateway_frame_t frame;
    uint8_t dataLen;

    can_abort_timed_out_tx(ch);
    if ((s_ms - ch->lastTxMs) < TX_PERIOD_MS)
    {
        return;
    }

    memset(&frame, 0, sizeof(frame));
    frame.magic = CAN_GATEWAY_MAGIC;
    frame.version = CAN_GATEWAY_VERSION;
    frame.channel = ch->index;
    frame.flags = ch->useFD ? CAN_GATEWAY_FLAG_FD : 0U;
    if (ch->useFD && ch->config.brs)
    {
        frame.flags |= CAN_GATEWAY_FLAG_BRS;
    }
    frame.dlc = ch->useFD ? CAN_FD_DLC : CAN_CLASSIC_DLC;
    frame.can_id = ch->txId;
    frame.timestamp = s_ms;
    frame.status = CAN_GATEWAY_STATUS_OK;

    dataLen = dlc_to_len(frame.dlc);
    frame.data[0] = (uint8_t)(ch->txCounter >> 24);
    frame.data[1] = (uint8_t)(ch->txCounter >> 16);
    frame.data[2] = (uint8_t)(ch->txCounter >> 8);
    frame.data[3] = (uint8_t)ch->txCounter;

    if (dataLen > 4U)
    {
        for (uint8_t i = 4U; i < dataLen; i++)
        {
            frame.data[i] = (uint8_t)i;
        }
    }

    if (can_start_tx(ch, &frame) == CAN_GATEWAY_STATUS_OK)
    {
        ch->txCounter++;
    }
}

static bool can_push_rx(can_channel_t *ch, const can_gateway_frame_t *frame)
{
    if (ch->rxQueued >= CAN_SERVICE_RX_RING_SIZE)
    {
        ch->status.rxDropCount++;
        ch->status.rxErrorCount++;
        return false;
    }

    ch->rxRing[ch->rxHead] = *frame;
    ch->rxHead = (uint8_t)((ch->rxHead + 1U) % CAN_SERVICE_RX_RING_SIZE);
    ch->rxQueued++;
    ch->status.rxQueued = ch->rxQueued;

    if (ch->status.rxQueued > ch->status.rxQueueWatermark)
    {
        ch->status.rxQueueWatermark = ch->status.rxQueued;
    }

    return true;
}

static void can_fill_rx_from_fd(can_channel_t *ch, const flexcan_fd_frame_t *rxFrame, can_gateway_frame_t *frame)
{
    uint8_t dataLen;

    memset(frame, 0, sizeof(*frame));
    frame->magic = CAN_GATEWAY_MAGIC;
    frame->version = CAN_GATEWAY_VERSION;
    frame->channel = ch->index;
    frame->status = CAN_GATEWAY_STATUS_OK;

    if (rxFrame->edl != 0U)
    {
        frame->flags |= CAN_GATEWAY_FLAG_FD;
    }
    if ((rxFrame->edl != 0U) && (rxFrame->brs != 0U))
    {
        frame->flags |= CAN_GATEWAY_FLAG_BRS;
    }
    if (rxFrame->format == (uint8_t)kFLEXCAN_FrameFormatExtend)
    {
        frame->flags |= CAN_GATEWAY_FLAG_EXTENDED_ID;
        frame->can_id = (rxFrame->id & 0x1FFFFFFFUL);
    }
    else
    {
        frame->can_id = rxFrame->id >> CAN_ID_STD_SHIFT;
    }
    frame->dlc = (uint8_t)rxFrame->length;
    frame->timestamp = s_ms;
    dataLen = dlc_to_len(frame->dlc);
    copy_words_to_bytes(frame->data, rxFrame->dataWord, dataLen);
}

static void can_fill_rx_from_classic(can_channel_t *ch, const flexcan_frame_t *rxFrame, can_gateway_frame_t *frame)
{
    uint8_t dataLen;

    memset(frame, 0, sizeof(*frame));
    frame->magic = CAN_GATEWAY_MAGIC;
    frame->version = CAN_GATEWAY_VERSION;
    frame->channel = ch->index;
    frame->status = CAN_GATEWAY_STATUS_OK;

    if (rxFrame->format == (uint8_t)kFLEXCAN_FrameFormatExtend)
    {
        frame->flags |= CAN_GATEWAY_FLAG_EXTENDED_ID;
        frame->can_id = (rxFrame->id & 0x1FFFFFFFUL);
    }
    else
    {
        frame->can_id = rxFrame->id >> CAN_ID_STD_SHIFT;
    }
    if (rxFrame->type == (uint8_t)kFLEXCAN_FrameTypeRemote)
    {
        frame->flags |= CAN_GATEWAY_FLAG_REMOTE;
    }
    frame->dlc = (uint8_t)rxFrame->length;
    frame->timestamp = s_ms;
    dataLen = dlc_to_len(frame->dlc);
    copy_words_to_bytes(frame->data, &rxFrame->dataWord0, dataLen);
}

static void can_store_rx_frame(can_channel_t *ch, const can_gateway_frame_t *frame)
{
    if (!can_frame_matches_filter(ch, frame))
    {
        return;
    }

    if (can_push_rx(ch, frame))
    {
        ch->status.rxCount++;
    }
}

static void can_handle_rx(can_channel_t *ch)
{
    can_gateway_frame_t frame;

#if CAN_SERVICE_USE_ENHANCED_RX_FIFO
    if (ch->useFD)
    {
        for (uint8_t i = 0U; i < CAN_SERVICE_EFIFO_BATCH_SIZE; i++)
        {
            can_fill_rx_from_fd(ch, &ch->rxFifoFrame[i], &frame);
            can_store_rx_frame(ch, &frame);
        }

        ch->rxFifoDone = false;
        can_start_rx(ch);
        return;
    }
#endif

    if (ch->useFD)
    {
        can_fill_rx_from_fd(ch, &ch->rxFrame.fd, &frame);
    }
    else
    {
        can_fill_rx_from_classic(ch, &ch->rxFrame.classic, &frame);
    }
    can_store_rx_frame(ch, &frame);
    ch->rxDone = false;
    can_start_rx(ch);
}

static void can_update_error_state(can_channel_t *ch)
{
    uint8_t txErr = 0U;
    uint8_t rxErr = 0U;
    uint64_t flags;
    uint32_t faultState;

    FLEXCAN_GetBusErrCount(ch->base, &txErr, &rxErr);
    flags = FLEXCAN_GetStatusFlags(ch->base);
    faultState = (uint32_t)((flags & (uint64_t)kFLEXCAN_FaultConfinementFlag) >> CAN_ESR1_FLTCONF_SHIFT);

    ch->status.txErrCounter = txErr;
    ch->status.rxErrCounter = rxErr;

    if (faultState >= 2U)
    {
        ch->status.state = CAN_SERVICE_STATE_BUS_OFF;
    }
    else if (faultState == 1U)
    {
        ch->status.state = CAN_SERVICE_STATE_ERROR_PASSIVE;
    }
    else if ((((uint32_t)flags & ((uint32_t)kFLEXCAN_TxErrorWarningFlag | (uint32_t)kFLEXCAN_RxErrorWarningFlag)) != 0U) ||
             (txErr >= CAN_ERROR_WARNING_THRESHOLD) || (rxErr >= CAN_ERROR_WARNING_THRESHOLD))
    {
        ch->status.state = CAN_SERVICE_STATE_WARNING;
    }
    else
    {
        ch->status.state = CAN_SERVICE_STATE_ERROR_ACTIVE;
    }
}

static void can_restart_rx_fifo(can_channel_t *ch)
{
#if CAN_SERVICE_USE_ENHANCED_RX_FIFO
    if (ch->useFD)
    {
        FLEXCAN_TransferAbortReceiveEnhancedFifo(ch->base, &ch->handle);
        ch->rxFifoDone = false;
        ch->rxFifoRestartNeeded = false;
        can_start_rx(ch);
    }
#else
    (void)ch;
#endif
}

static void can_print_status(const can_channel_t *ch)
{
    PRINTF("CAN%d: status rx=%u tx_start=%u tx_done=%u txq=%u rxq=%u tx_drop=%u rx_drop=%u err=%u fifo_ovf=%u last=0x%x\r\n",
           ch->index,
           (unsigned)ch->status.rxCount,
           (unsigned)ch->status.txStartCount,
           (unsigned)ch->status.txDoneCount,
           (unsigned)ch->status.txQueued,
           (unsigned)ch->status.rxQueued,
           (unsigned)ch->status.txDropCount,
           (unsigned)ch->status.rxDropCount,
           (unsigned)(ch->status.txErrorCount + ch->status.rxErrorCount + ch->status.txTimeoutCount),
           (unsigned)ch->status.rxFifoOverflowCount,
           (unsigned)ch->status.lastErrorStatus);
}

bool can_service_init(uint32_t activeMask)
{
    if (s_initialized)
    {
        return true;
    }

    memset(s_can, 0, sizeof(s_can));
    s_activeMask = activeMask & ((1UL << CAN_SERVICE_CHANNEL_COUNT) - 1UL);

    for (uint8_t i = 0U; i < CAN_SERVICE_CHANNEL_COUNT; i++)
    {
        can_channel_t *ch = &s_can[i];

        ch->base = s_canConfig[i].base;
        ch->clkFreq = CLOCK_GetFreq(s_canConfig[i].clkName);
        ch->txId = s_canConfig[i].txId;
        ch->index = i;
        ch->config = can_default_config(can_channel_enabled(i));
        ch->status.state = CAN_SERVICE_STATE_ERROR_ACTIVE;
        can_sync_channel_from_config(ch);
        can_reset_queues(ch);
    }

    PRINTF("CAN service: active_mask=0x%x\r\n", (unsigned)s_activeMask);

    for (uint8_t i = 0U; i < CAN_SERVICE_CHANNEL_COUNT; i++)
    {
        can_channel_t *ch = &s_can[i];

        if (!ch->status.enabled)
        {
            continue;
        }

        can_init_channel(ch);

        if (ch->useFD)
        {
            PRINTF("CAN%d: enabled CAN FD %ukbps/%ukbps BRS %s TX=0x%x EFIFO batch=%u watermark=%u\r\n",
                   i,
                   (unsigned)(ch->bitRate / 1000U),
                   (unsigned)(ch->bitRateFD / 1000U),
                   ch->config.brs ? "on" : "off",
                   (unsigned)ch->txId,
                   (unsigned)CAN_SERVICE_EFIFO_BATCH_SIZE,
                   (unsigned)CAN_SERVICE_EFIFO_WATERMARK);
        }
        else
        {
            PRINTF("CAN%d: enabled Classic CAN %ukbps TX=0x%x\r\n",
                   i,
                   (unsigned)(ch->bitRate / 1000U),
                   (unsigned)ch->txId);
        }
    }

    s_initialized = true;
    return true;
}

void can_service_poll(void)
{
    if (!s_initialized)
    {
        return;
    }

    for (uint8_t i = 0U; i < CAN_SERVICE_CHANNEL_COUNT; i++)
    {
        can_channel_t *ch = &s_can[i];

        if (!ch->status.enabled)
        {
            continue;
        }

        can_update_error_state(ch);

#if CAN_SERVICE_ENABLE_PERIODIC_TX
        can_send_periodic(ch);
#else
        can_abort_timed_out_tx(ch);
        can_process_tx_queue(ch);
#endif

        if (ch->rxFifoRestartNeeded)
        {
            can_restart_rx_fifo(ch);
        }

        if (ch->rxFifoDone)
        {
            can_handle_rx(ch);
        }

        if (ch->rxDone)
        {
            can_handle_rx(ch);
        }
    }

    if ((s_ms - s_lastStatusMs) >= STATUS_PERIOD_MS)
    {
        s_lastStatusMs = s_ms;
        for (uint8_t i = 0U; i < CAN_SERVICE_CHANNEL_COUNT; i++)
        {
            if (s_can[i].status.enabled)
            {
                can_print_status(&s_can[i]);
            }
        }
    }
}

uint32_t can_service_send(uint8_t channel, const can_gateway_frame_t *frame)
{
    if ((frame == NULL) || (channel >= CAN_SERVICE_CHANNEL_COUNT) || (frame->channel != channel))
    {
        return CAN_GATEWAY_STATUS_INVALID_PACKET;
    }

    if (!can_channel_enabled(channel))
    {
        return CAN_GATEWAY_STATUS_DISABLED_CHANNEL;
    }

    return can_enqueue_tx(&s_can[channel], frame);
}

bool can_service_read(uint8_t channel, can_gateway_frame_t *frame)
{
    can_channel_t *ch;

    if ((frame == NULL) || !can_channel_enabled(channel))
    {
        return false;
    }

    ch = &s_can[channel];
    if (ch->rxQueued == 0U)
    {
        return false;
    }

    *frame = ch->rxRing[ch->rxTail];
    ch->rxTail = (uint8_t)((ch->rxTail + 1U) % CAN_SERVICE_RX_RING_SIZE);
    ch->rxQueued--;
    ch->status.rxQueued = ch->rxQueued;
    return true;
}

can_service_status_t can_service_get_status(uint8_t channel)
{
    can_service_status_t emptyStatus;

    memset(&emptyStatus, 0, sizeof(emptyStatus));

    if (channel >= CAN_SERVICE_CHANNEL_COUNT)
    {
        return emptyStatus;
    }

    return s_can[channel].status;
}

can_service_config_t can_service_get_config(uint8_t channel)
{
    can_service_config_t emptyConfig;

    memset(&emptyConfig, 0, sizeof(emptyConfig));

    if (channel >= CAN_SERVICE_CHANNEL_COUNT)
    {
        return emptyConfig;
    }

    return s_can[channel].config;
}

uint32_t can_service_set_config(uint8_t channel, const can_service_config_t *config)
{
    uint32_t status = can_validate_config(channel, config);

    if (status == CAN_SERVICE_CONFIG_OK)
    {
        status = can_apply_config(channel, config);
    }

    s_lastConfigStatus = status;
    return status;
}

uint32_t can_service_get_last_config_status(void)
{
    return s_lastConfigStatus;
}

void can_service_tick_1ms(void)
{
    s_ms++;
}
