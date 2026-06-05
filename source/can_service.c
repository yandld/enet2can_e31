/*
 * can_service.c - CAN service boundary for the six-channel gateway
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Fully polled, no FlexCAN interrupts, and one identical RX mechanism on every
 * channel: a bank of individual Rx message buffers scanned each poll. (CAN0's
 * Enhanced Rx FIFO is intentionally NOT used so all six channels are configured
 * identically and are easy to validate; only FlexCAN0 even has that FIFO.)
 *   - RX: a bank of accept-all Rx message buffers. Individual Rx masking + queue
 *     (MCR[IRMQ]) is ENABLED so the bank acts as a reception queue: a new frame is
 *     stored in the next free mailbox instead of overwriting the lowest one.
 *     Without IRMQ the lowest mailbox captures every frame and is overwritten in
 *     place (effective depth 1); with IRMQ the depth equals the free mailbox count.
 *     The bank is 5 deep on every channel (the 64-byte FD ceiling of the smallest
 *     instances CAN3/4/5); the real burst depth must still be validated on the bench.
 *   - TX: two mailboxes per channel (polled double-buffer); frames are written with
 *     FLEXCAN_Write*TxMb and completion/timeout is detected by polling the IFLAG.
 *   - Errors: bus state is polled every 10 ms and turned into SocketCAN error frames.
 * Because there is no ISR, all queue/mailbox/error state is single-context.
 */

#include "can_service.h"

#include <string.h>

#include "fsl_clock.h"
#include "fsl_debug_console.h"
#include "fsl_flexcan.h"

#define CAN_TX_ID_BASE 0x100U
#define TX_TIMEOUT_MS 10U
#define FD_PAYLOAD_SIZE kFLEXCAN_64BperMB
#define CAN_SERVICE_ERROR_POLL_PERIOD_MS 10U
#define CAN_ERROR_WARNING_THRESHOLD 96U
#define CAN_MIN_NOMINAL_BITRATE 50000U
#define CAN_MAX_NOMINAL_BITRATE 1000000U
#define CAN_MIN_DATA_BITRATE 500000U
#define CAN_MAX_DATA_BITRATE 5000000U
#define CAN_ERR_TX_TIMEOUT 0x00000001UL
#define CAN_ERR_CRTL 0x00000004UL
#define CAN_ERR_BUSOFF 0x00000040UL
#define CAN_ERR_BUSERROR 0x00000080UL
#define CAN_ERR_CRTL_RX_OVERFLOW 0x01U
#define CAN_ERR_CRTL_TX_OVERFLOW 0x02U
#define CAN_ERR_CRTL_RX_WARNING 0x04U
#define CAN_ERR_CRTL_TX_WARNING 0x08U
#define CAN_ERR_CRTL_RX_PASSIVE 0x10U
#define CAN_ERR_CRTL_TX_PASSIVE 0x20U

typedef struct
{
    CAN_Type *base;
    clock_name_t clkName;
    uint32_t txId;
    uint8_t txMbFirst; /* first TX message buffer index */
    uint8_t txMbCount; /* number of TX message buffers */
    uint8_t rxMbFirst; /* first RX message buffer index */
    uint8_t rxMbCount; /* RX message buffers in the bank */
} can_channel_config_t;

typedef struct
{
    CAN_Type *base;
    const can_channel_config_t *plan;
    uint32_t clkFreq;
    uint32_t bitRate;
    bool useFD;
    uint32_t bitRateFD;
    uint32_t txId;
    uint8_t index;
    can_service_config_t config;

    uint8_t txMbBusy[CAN_SERVICE_MAX_TX_MB];
    uint32_t txStartMs[CAN_SERVICE_MAX_TX_MB];
    uint32_t lastErrorPollMs;

    bool errorFramePending;
    uint32_t pendingErrorCanId;
    uint8_t pendingErrorCtrl;

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
} can_channel_t;

static volatile uint32_t s_ms;
static bool s_initialized;
static uint32_t s_activeMask;
static uint32_t s_lastConfigStatus = CAN_SERVICE_CONFIG_OK;
static can_channel_t s_can[CAN_SERVICE_CHANNEL_COUNT];
/* UDP-in -> CAN-MB-write latency: stamped in the UDP callback, measured here when
 * the frame is actually written to a TX mailbox (the board's "CAN-out" instant). */
static latency_stat_t s_udpToCanLatency;

/*
 * Identical message-buffer plan on all six instances so every channel is configured
 * and validated the same way and nothing depends on FlexCAN0-only features. The
 * 64-byte FD payload packs only 7 mailboxes per 512-byte block, and the smallest
 * instances (CAN3/4/5) have exactly one block, so 7 MBs is the common ceiling:
 * 2 TX (idx 0..1) + 5 RX (idx 2..6) on every channel.
 */
#define CAN_TX_MB_COUNT 2U
#define CAN_RX_MB_COUNT 5U
/* 64-byte FD packs 7 MBs per 512-byte block; CAN3/4/5 have exactly one block. */
#define CAN_FD_MB_PER_INSTANCE 7U

_Static_assert((CAN_TX_MB_COUNT + CAN_RX_MB_COUNT) <= CAN_FD_MB_PER_INSTANCE,
               "uniform FlexCAN MB plan exceeds the smallest instance's 64-byte FD capacity");
_Static_assert(CAN_TX_MB_COUNT <= CAN_SERVICE_MAX_TX_MB,
               "CAN_TX_MB_COUNT exceeds the per-channel TX bookkeeping array size");

static const can_channel_config_t s_canConfig[CAN_SERVICE_CHANNEL_COUNT] =
{
    { FLEXCAN_0, kCLOCK_Flexcan0Clk, CAN_TX_ID_BASE + 0U, 0U, CAN_TX_MB_COUNT, CAN_TX_MB_COUNT, CAN_RX_MB_COUNT },
    { FLEXCAN_1, kCLOCK_Flexcan1Clk, CAN_TX_ID_BASE + 1U, 0U, CAN_TX_MB_COUNT, CAN_TX_MB_COUNT, CAN_RX_MB_COUNT },
    { FLEXCAN_2, kCLOCK_Flexcan2Clk, CAN_TX_ID_BASE + 2U, 0U, CAN_TX_MB_COUNT, CAN_TX_MB_COUNT, CAN_RX_MB_COUNT },
    { FLEXCAN_3, kCLOCK_Flexcan3Clk, CAN_TX_ID_BASE + 3U, 0U, CAN_TX_MB_COUNT, CAN_TX_MB_COUNT, CAN_RX_MB_COUNT },
    { FLEXCAN_4, kCLOCK_Flexcan4Clk, CAN_TX_ID_BASE + 4U, 0U, CAN_TX_MB_COUNT, CAN_TX_MB_COUNT, CAN_RX_MB_COUNT },
    { FLEXCAN_5, kCLOCK_Flexcan5Clk, CAN_TX_ID_BASE + 5U, 0U, CAN_TX_MB_COUNT, CAN_TX_MB_COUNT, CAN_RX_MB_COUNT }
};

static void can_init_channel(can_channel_t *ch);
static bool can_push_rx(can_channel_t *ch, const can_gateway_frame_t *frame, bool isError);

static uint8_t dlc_to_len(uint8_t dlc)
{
    return (dlc <= CAN_FD_DLC) ? (uint8_t)DLC_LENGTH_DECODE(dlc) : CAN_GATEWAY_MAX_DATA_LEN;
}

static uint8_t channel_max_mb(const can_channel_config_t *plan)
{
    uint8_t txEnd = (uint8_t)(plan->txMbFirst + plan->txMbCount);
    uint8_t rxEnd = (uint8_t)(plan->rxMbFirst + plan->rxMbCount);
    return (rxEnd > txEnd) ? rxEnd : txEnd;
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
    ch->errorFramePending = false;
    ch->pendingErrorCanId = 0U;
    ch->pendingErrorCtrl = 0U;
    ch->lastErrorPollMs = 0U;

    for (uint8_t i = 0U; i < CAN_SERVICE_MAX_TX_MB; i++)
    {
        ch->txMbBusy[i] = 0U;
        ch->txStartMs[i] = 0U;
    }
}

static void can_sync_channel_from_config(can_channel_t *ch)
{
    ch->bitRate = ch->config.bitRate;
    ch->useFD = ch->config.useFD;
    ch->bitRateFD = ch->config.useFD ? ch->config.bitRateFD : 0U;
    ch->status.enabled = ch->config.enabled;
    ch->status.useFD = ch->config.useFD;
    ch->status.enhancedRxFifo = false; /* uniform Rx MB bank on every channel */
    ch->status.bitRate = ch->config.bitRate;
    ch->status.bitRateFD = ch->bitRateFD;
    ch->status.rxCapacity = CAN_SERVICE_RX_RING_SIZE;
    ch->status.txCapacity = CAN_SERVICE_TX_QUEUE_SIZE;
    ch->status.rxDrainMax = CAN_SERVICE_RX_DRAIN_MAX;
    ch->status.rxHwSlots = ch->plan->rxMbCount;
    ch->status.txMbCount = ch->plan->txMbCount;
    ch->status.rxMbCount = ch->plan->rxMbCount;
}

static uint32_t can_validate_config(uint8_t channel, const can_service_config_t *config)
{
    if (channel >= CAN_SERVICE_CHANNEL_COUNT)
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

    /* No interrupts/transfers are armed; a full module reset aborts any pending TX. */
    FLEXCAN_Deinit(ch->base);
}

static uint32_t can_apply_config(uint8_t channel, const can_service_config_t *config)
{
    can_channel_t *ch = &s_can[channel];

    /* Single-context (no ISR): reconfiguration cannot race interrupt handlers. */
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

static uint8_t can_error_ctrl_bits(uint8_t txErr, uint8_t rxErr)
{
    uint8_t ctrl = 0U;

    if (rxErr >= 128U)
    {
        ctrl |= CAN_ERR_CRTL_RX_PASSIVE;
    }
    else if (rxErr >= CAN_ERROR_WARNING_THRESHOLD)
    {
        ctrl |= CAN_ERR_CRTL_RX_WARNING;
    }

    if (txErr >= 128U)
    {
        ctrl |= CAN_ERR_CRTL_TX_PASSIVE;
    }
    else if (txErr >= CAN_ERROR_WARNING_THRESHOLD)
    {
        ctrl |= CAN_ERR_CRTL_TX_WARNING;
    }

    return ctrl;
}

static void can_read_error_counters(can_channel_t *ch, uint8_t *txErr, uint8_t *rxErr)
{
    uint8_t tx = 0U;
    uint8_t rx = 0U;

    FLEXCAN_GetBusErrCount(ch->base, &tx, &rx);
    ch->status.txErrCounter = tx;
    ch->status.rxErrCounter = rx;

    if (txErr != NULL)
    {
        *txErr = tx;
    }
    if (rxErr != NULL)
    {
        *rxErr = rx;
    }
}

/* Accumulate a pending SocketCAN error event (merged until flushed into the ring). */
static void can_queue_error_event(can_channel_t *ch, uint32_t canId, uint8_t ctrl)
{
    ch->pendingErrorCanId |= canId;
    ch->pendingErrorCtrl |= ctrl;
    ch->errorFramePending = true;
}

static bool can_push_error_frame(can_channel_t *ch, uint32_t canId, uint8_t ctrl)
{
    can_gateway_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.channel = ch->index;
    frame.flags = CAN_GATEWAY_FLAG_ERROR;
    frame.dlc = CAN_CLASSIC_DLC;
    frame.can_id = canId;
    frame.timestamp = s_ms;
    frame.status = CAN_GATEWAY_STATUS_OK;
    frame.data[1] = ctrl;
    frame.data[6] = ch->status.txErrCounter;
    frame.data[7] = ch->status.rxErrCounter;

    return can_push_rx(ch, &frame, true);
}

/* Push the pending error event into the RX ring; keep it pending if the ring is full. */
static void can_flush_pending_error_frame(can_channel_t *ch)
{
    if (!ch->errorFramePending)
    {
        return;
    }

    if (can_push_error_frame(ch, ch->pendingErrorCanId, ch->pendingErrorCtrl))
    {
        ch->pendingErrorCanId = 0U;
        ch->pendingErrorCtrl = 0U;
        ch->errorFramePending = false;
    }
}

/*
 * Log the bit rates ACTUALLY programmed into the timing registers. The PE clock
 * may not divide the requested rate cleanly (e.g. 48 MHz cannot make exactly
 * 5 Mbit/s), and FLEXCAN_FDCalculateImprovedTimingValues only accepts exact
 * divisors -- on failure the SDK keeps the default timing, so the real data
 * phase can differ from the request. Init-only (not a hot path). Bit time in TQ
 * = 1 (sync) + propSeg + (phaseSeg1+1) + (phaseSeg2+1); rate = clk / (preDiv * tq).
 */
static void can_log_achieved_bitrate(const can_channel_t *ch, const flexcan_timing_config_t *t, bool exact)
{
    uint32_t nTq = (t->preDivider + 1U) * (t->propSeg + t->phaseSeg1 + t->phaseSeg2 + 3U);
    uint32_t dTq = (t->fpreDivider + 1U) * (t->fpropSeg + t->fphaseSeg1 + t->fphaseSeg2 + 3U);
    uint32_t nominal = (nTq != 0U) ? (ch->clkFreq / nTq) : 0U;
    uint32_t data = (dTq != 0U) ? (ch->clkFreq / dTq) : 0U;

    PRINTF("CAN%u: PE clk=%ukHz achieved nominal=%ukbps data=%ukbps (requested %u/%ukbps)%s\r\n",
           (unsigned)ch->index,
           (unsigned)(ch->clkFreq / 1000U),
           (unsigned)(nominal / 1000U),
           (unsigned)(data / 1000U),
           (unsigned)(ch->bitRate / 1000U),
           (unsigned)(ch->bitRateFD / 1000U),
           exact ? "" : " [WARN: clock does not divide rate exactly -- using default timing]");
}

static void can_init_channel(can_channel_t *ch)
{
    flexcan_config_t cfg;
    flexcan_rx_mb_config_t mbRxCfg;
    flexcan_timing_config_t timing;
    const can_channel_config_t *plan = ch->plan;

    FLEXCAN_GetDefaultConfig(&cfg);
    cfg.bitRate = ch->bitRate;
    cfg.maxMbNum = channel_max_mb(plan);
    cfg.disableSelfReception = true;
    cfg.enableIndividMask = true; /* MCR[IRMQ]=1: the Rx MB bank acts as a reception queue */

    memset(&timing, 0, sizeof(timing));

    if (ch->useFD)
    {
        bool exact;

        cfg.bitRateFD = ch->bitRateFD;
        exact = FLEXCAN_FDCalculateImprovedTimingValues(ch->base, cfg.bitRate, cfg.bitRateFD, ch->clkFreq, &timing);
        if (exact)
        {
            memcpy(&cfg.timingConfig, &timing, sizeof(timing));
        }
        FLEXCAN_FDInit(ch->base, &cfg, ch->clkFreq, FD_PAYLOAD_SIZE, true);
        can_log_achieved_bitrate(ch, &cfg.timingConfig, exact);
    }
    else
    {
        if (FLEXCAN_CalculateImprovedTimingValues(ch->base, cfg.bitRate, ch->clkFreq, &timing))
        {
            memcpy(&cfg.timingConfig, &timing, sizeof(timing));
        }
        FLEXCAN_Init(ch->base, &cfg, ch->clkFreq);
    }

    /* No handle / no interrupts: everything is polled (see file header). With IRMQ
     * enabled the per-mailbox individual mask (RXIMR) replaces the global mask, so
     * each Rx mailbox is set accept-all below and the software filter applies on read. */

    mbRxCfg.format = kFLEXCAN_FrameFormatStandard;
    mbRxCfg.type = kFLEXCAN_FrameTypeData;
    mbRxCfg.id = FLEXCAN_ID_STD(0U);

    /* TX message buffers. */
    for (uint8_t i = 0U; i < plan->txMbCount; i++)
    {
        uint8_t mbIdx = (uint8_t)(plan->txMbFirst + i);

        if (ch->useFD)
        {
            FLEXCAN_SetFDTxMbConfig(ch->base, mbIdx, true);
        }
        else
        {
            FLEXCAN_SetTxMbConfig(ch->base, mbIdx, true);
        }
    }

    /* RX message-buffer bank: accept-all individual mask + same id/format on every
     * mailbox so the IRMQ reception queue stores each new frame in the next free one. */
    for (uint8_t i = 0U; i < plan->rxMbCount; i++)
    {
        uint8_t mbIdx = (uint8_t)(plan->rxMbFirst + i);

        FLEXCAN_SetRxIndividualMask(ch->base, mbIdx, 0U); /* accept-all (mask 0 = don't care) */

        if (ch->useFD)
        {
            FLEXCAN_SetFDRxMbConfig(ch->base, mbIdx, &mbRxCfg, true);
        }
        else
        {
            FLEXCAN_SetRxMbConfig(ch->base, mbIdx, &mbRxCfg, true);
        }
    }
}

static bool can_validate_tx_frame(const can_channel_t *ch, const can_gateway_frame_t *frame)
{
    uint8_t knownFlags = CAN_GATEWAY_FLAG_FD | CAN_GATEWAY_FLAG_BRS | CAN_GATEWAY_FLAG_EXTENDED_ID |
                          CAN_GATEWAY_FLAG_REMOTE | CAN_GATEWAY_FLAG_ERROR;
    bool isExtended;
    bool isFD;
    uint8_t dataLen;

    if ((ch == NULL) || (frame == NULL) || (frame->channel >= CAN_SERVICE_CHANNEL_COUNT) ||
        (frame->dlc > CAN_FD_DLC) || ((frame->flags & CAN_GATEWAY_FLAG_ERROR) != 0U) ||
        ((frame->flags & (uint8_t)~knownFlags) != 0U))
    {
        return false;
    }

    isExtended = ((frame->flags & CAN_GATEWAY_FLAG_EXTENDED_ID) != 0U);
    isFD = ((frame->flags & CAN_GATEWAY_FLAG_FD) != 0U);
    dataLen = dlc_to_len(frame->dlc);

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

static bool can_prepare_tx_frame(can_channel_t *ch, const can_gateway_frame_t *frame)
{
    uint8_t dataLen = dlc_to_len(frame->dlc);
    bool isExtended = ((frame->flags & CAN_GATEWAY_FLAG_EXTENDED_ID) != 0U);
    bool isFD = ((frame->flags & CAN_GATEWAY_FLAG_FD) != 0U);

    if (!can_validate_tx_frame(ch, frame))
    {
        return false;
    }

    memset(&ch->txFrame, 0, sizeof(ch->txFrame));

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
    }
    else
    {
        ch->txFrame.classic.id = isExtended ? FLEXCAN_ID_EXT(frame->can_id) : FLEXCAN_ID_STD(frame->can_id);
        ch->txFrame.classic.format = isExtended ? (uint8_t)kFLEXCAN_FrameFormatExtend : (uint8_t)kFLEXCAN_FrameFormatStandard;
        ch->txFrame.classic.type = ((frame->flags & CAN_GATEWAY_FLAG_REMOTE) != 0U) ?
                                   (uint8_t)kFLEXCAN_FrameTypeRemote : (uint8_t)kFLEXCAN_FrameTypeData;
        ch->txFrame.classic.length = dataLen;
        copy_bytes_to_words(&ch->txFrame.classic.dataWord0, frame->data, dataLen);
    }

    return true;
}

static int8_t can_find_free_tx_mb(const can_channel_t *ch)
{
    for (uint8_t i = 0U; i < ch->plan->txMbCount; i++)
    {
        if (ch->txMbBusy[i] == 0U)
        {
            return (int8_t)i;
        }
    }
    return -1;
}

/* Write a queued TX frame into a free TX mailbox (polled, no interrupt). */
static uint32_t can_start_tx(can_channel_t *ch, uint8_t localMb, const can_gateway_frame_t *frame)
{
    uint8_t mbIdx = (uint8_t)(ch->plan->txMbFirst + localMb);
    status_t status;

    if (!can_prepare_tx_frame(ch, frame))
    {
        ch->status.txErrorCount++;
        return CAN_GATEWAY_STATUS_INVALID_PACKET;
    }

    if (ch->useFD)
    {
        status = FLEXCAN_WriteFDTxMb(ch->base, mbIdx, &ch->txFrame.fd);
    }
    else
    {
        status = FLEXCAN_WriteTxMb(ch->base, mbIdx, &ch->txFrame.classic);
    }

    if (status != kStatus_Success)
    {
        /* Mailbox still transmitting a previous frame; retry next poll. */
        ch->status.txBusyCount++;
        return CAN_GATEWAY_STATUS_CAN_TX_BUSY;
    }

    ch->txMbBusy[localMb] = 1U;
    ch->txStartMs[localMb] = s_ms;
    ch->status.txStartCount++;
    if (frame->ingress_cycles != 0U)
    {
        latency_stat_add(&s_udpToCanLatency, (uint32_t)(latency_cycle_now() - frame->ingress_cycles));
    }
    return CAN_GATEWAY_STATUS_OK;
}

/* Poll TX mailbox completion flags and free finished mailboxes. */
static void can_poll_tx_done(can_channel_t *ch)
{
    for (uint8_t i = 0U; i < ch->plan->txMbCount; i++)
    {
        uint8_t mbIdx = (uint8_t)(ch->plan->txMbFirst + i);

        if ((ch->txMbBusy[i] != 0U) && (FLEXCAN_GetMbStatusFlags(ch->base, (uint64_t)1U << mbIdx) != 0U))
        {
            FLEXCAN_ClearMbStatusFlags(ch->base, (uint64_t)1U << mbIdx);
            ch->txMbBusy[i] = 0U;
            ch->status.txDoneCount++;
        }
    }
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
    while (ch->txQueued > 0U)
    {
        int8_t localMb = can_find_free_tx_mb(ch);
        uint32_t status;

        if (localMb < 0)
        {
            break; /* all TX mailboxes in flight */
        }

        status = can_start_tx(ch, (uint8_t)localMb, &ch->txQueue[ch->txTail]);
        if (status == CAN_GATEWAY_STATUS_OK)
        {
            can_pop_tx(ch);
        }
        else if (status == CAN_GATEWAY_STATUS_CAN_TX_BUSY)
        {
            break; /* mailbox not ready; retry next poll */
        }
        else
        {
            ch->status.txDropCount++;
            can_pop_tx(ch); /* invalid/unsendable frame: drop and move on */
        }
    }
}

static void can_abort_timed_out_tx(can_channel_t *ch)
{
    for (uint8_t i = 0U; i < ch->plan->txMbCount; i++)
    {
        uint8_t mbIdx;

        if ((ch->txMbBusy[i] == 0U) || ((s_ms - ch->txStartMs[i]) < TX_TIMEOUT_MS))
        {
            continue;
        }

        /* Re-configuring the mailbox to inactive aborts the stuck transmission. */
        mbIdx = (uint8_t)(ch->plan->txMbFirst + i);
        if (ch->useFD)
        {
            FLEXCAN_SetFDTxMbConfig(ch->base, mbIdx, true);
        }
        else
        {
            FLEXCAN_SetTxMbConfig(ch->base, mbIdx, true);
        }
        FLEXCAN_ClearMbStatusFlags(ch->base, (uint64_t)1U << mbIdx);

        ch->txMbBusy[i] = 0U;
        ch->status.txTimeoutCount++;
        ch->status.txErrorCount++;
        can_queue_error_event(ch, CAN_ERR_TX_TIMEOUT, CAN_ERR_CRTL_TX_OVERFLOW);
    }
}

/* Data frames respect the error headroom; error events may use the whole ring. */
static bool can_push_rx(can_channel_t *ch, const can_gateway_frame_t *frame, bool isError)
{
    uint8_t limit = isError ? CAN_SERVICE_RX_RING_SIZE
                            : (uint8_t)(CAN_SERVICE_RX_RING_SIZE - CAN_SERVICE_RX_ERROR_HEADROOM);

    if (ch->rxQueued >= limit)
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
    /* Classic frames carry at most 8 bytes; a DLC of 9..15 still means 8 and must
     * not over-read the 8-byte classic mailbox data union. */
    frame->dlc = (rxFrame->length > CAN_CLASSIC_DLC) ? CAN_CLASSIC_DLC : (uint8_t)rxFrame->length;
    frame->timestamp = s_ms;
    dataLen = frame->dlc;
    copy_words_to_bytes(frame->data, &rxFrame->dataWord0, dataLen);
}

static void can_store_rx_frame(can_channel_t *ch, const can_gateway_frame_t *frame)
{
    if (!can_frame_matches_filter(ch, frame))
    {
        return;
    }

    if (can_push_rx(ch, frame, false))
    {
        ch->status.rxCount++;
    }
}

/* Drain the channel's Rx message-buffer bank: read+clear every mailbox with data. */
static void can_drain_mb_bank(can_channel_t *ch)
{
    can_gateway_frame_t frame;
    uint8_t budget = CAN_SERVICE_RX_DRAIN_MAX;

    for (uint8_t i = 0U; (i < ch->plan->rxMbCount) && (budget > 0U); i++)
    {
        uint8_t mbIdx = (uint8_t)(ch->plan->rxMbFirst + i);
        status_t status;

        if (FLEXCAN_GetMbStatusFlags(ch->base, (uint64_t)1U << mbIdx) == 0U)
        {
            continue;
        }

        if (ch->useFD)
        {
            status = FLEXCAN_ReadFDRxMb(ch->base, mbIdx, &ch->rxFrame.fd);
        }
        else
        {
            status = FLEXCAN_ReadRxMb(ch->base, mbIdx, &ch->rxFrame.classic);
        }
        FLEXCAN_ClearMbStatusFlags(ch->base, (uint64_t)1U << mbIdx);

        if ((status != kStatus_Success) && (status != kStatus_FLEXCAN_RxOverflow))
        {
            continue;
        }

        if (status == kStatus_FLEXCAN_RxOverflow)
        {
            /* A frame was overwritten on this MB before it was read (lost). Count it in
             * the dedicated overflow counter (rx_fifo_overflow, the documented acceptance
             * field) as well as the aggregate error counter. */
            ch->status.rxFifoOverflowCount++;
            ch->status.rxErrorCount++;
        }

        if (ch->useFD)
        {
            can_fill_rx_from_fd(ch, &ch->rxFrame.fd, &frame);
        }
        else
        {
            can_fill_rx_from_classic(ch, &ch->rxFrame.classic, &frame);
        }
        frame.ingress_cycles = latency_cycle_now(); /* CAN-in instant (MB read) for latency */
        can_store_rx_frame(ch, &frame);
        budget--;
    }
}

static void can_handle_rx(can_channel_t *ch)
{
    can_drain_mb_bank(ch);
}

static void can_update_error_state(can_channel_t *ch)
{
    uint8_t txErr = 0U;
    uint8_t rxErr = 0U;
    uint64_t flags;
    uint32_t faultState;
    can_service_state_t newState;

    can_read_error_counters(ch, &txErr, &rxErr);
    flags = FLEXCAN_GetStatusFlags(ch->base);
    faultState = (uint32_t)((flags & (uint64_t)kFLEXCAN_FaultConfinementFlag) >> CAN_ESR1_FLTCONF_SHIFT);

    if (faultState >= 2U)
    {
        newState = CAN_SERVICE_STATE_BUS_OFF;
    }
    else if (faultState == 1U)
    {
        newState = CAN_SERVICE_STATE_ERROR_PASSIVE;
    }
    else if ((((uint32_t)flags & ((uint32_t)kFLEXCAN_TxErrorWarningFlag | (uint32_t)kFLEXCAN_RxErrorWarningFlag)) != 0U) ||
             (txErr >= CAN_ERROR_WARNING_THRESHOLD) || (rxErr >= CAN_ERROR_WARNING_THRESHOLD))
    {
        newState = CAN_SERVICE_STATE_WARNING;
    }
    else
    {
        newState = CAN_SERVICE_STATE_ERROR_ACTIVE;
    }

    /* Emit a SocketCAN error frame when the bus condition worsens (polled, 10 ms). */
    if (newState > ch->status.state)
    {
        uint8_t ctrl = can_error_ctrl_bits(txErr, rxErr);

        if (newState == CAN_SERVICE_STATE_BUS_OFF)
        {
            ch->status.busOffCount++;
            can_queue_error_event(ch, CAN_ERR_BUSOFF | CAN_ERR_BUSERROR, ctrl);
        }
        else
        {
            can_queue_error_event(ch, CAN_ERR_BUSERROR | CAN_ERR_CRTL, ctrl);
        }
        ch->status.lastErrorStatus = (uint32_t)flags;
    }

    ch->status.state = newState;
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
        ch->plan = &s_canConfig[i];
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
            PRINTF("CAN%d: FD %ukbps/%ukbps BRS %s rx=mb-bank(%u) tx_mb=%u\r\n",
                   i,
                   (unsigned)(ch->bitRate / 1000U),
                   (unsigned)(ch->bitRateFD / 1000U),
                   ch->config.brs ? "on" : "off",
                   (unsigned)ch->plan->rxMbCount,
                   (unsigned)ch->plan->txMbCount);
        }
        else
        {
            PRINTF("CAN%d: Classic %ukbps rx=mb-bank(%u) tx_mb=%u\r\n",
                   i,
                   (unsigned)(ch->bitRate / 1000U),
                   (unsigned)ch->plan->rxMbCount,
                   (unsigned)ch->plan->txMbCount);
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

        if ((s_ms - ch->lastErrorPollMs) >= CAN_SERVICE_ERROR_POLL_PERIOD_MS)
        {
            ch->lastErrorPollMs = s_ms;
            can_update_error_state(ch);
        }

        can_poll_tx_done(ch);
        can_abort_timed_out_tx(ch);
        can_process_tx_queue(ch);
        can_handle_rx(ch);
        can_flush_pending_error_frame(ch);
    }
}

uint32_t can_service_send(uint8_t channel, const can_gateway_frame_t *frame)
{
    can_channel_t *ch;

    if ((frame == NULL) || (channel >= CAN_SERVICE_CHANNEL_COUNT) || (frame->channel != channel))
    {
        return CAN_GATEWAY_STATUS_INVALID_PACKET;
    }

    if (!can_channel_enabled(channel))
    {
        return CAN_GATEWAY_STATUS_DISABLED_CHANNEL;
    }

    ch = &s_can[channel];

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

    /* Kick the queue so the first frame is written without waiting a poll period. */
    can_process_tx_queue(ch);
    return CAN_GATEWAY_STATUS_OK;
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

uint16_t can_service_rx_available(uint8_t channel)
{
    if (!can_channel_enabled(channel))
    {
        return 0U;
    }

    return s_can[channel].rxQueued;
}

bool can_service_peek(uint8_t channel, uint16_t offset, can_gateway_frame_t *frame)
{
    can_channel_t *ch;
    uint8_t idx;

    if ((frame == NULL) || !can_channel_enabled(channel))
    {
        return false;
    }

    ch = &s_can[channel];
    if (offset >= ch->rxQueued)
    {
        return false;
    }

    idx = (uint8_t)((ch->rxTail + offset) % CAN_SERVICE_RX_RING_SIZE);
    *frame = ch->rxRing[idx];
    return true;
}

void can_service_consume(uint8_t channel, uint16_t count)
{
    can_channel_t *ch;

    if (!can_channel_enabled(channel))
    {
        return;
    }

    ch = &s_can[channel];
    if (count > ch->rxQueued)
    {
        count = ch->rxQueued;
    }

    ch->rxTail = (uint8_t)((ch->rxTail + count) % CAN_SERVICE_RX_RING_SIZE);
    ch->rxQueued = (uint8_t)(ch->rxQueued - count);
    ch->status.rxQueued = ch->rxQueued;
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

latency_stat_t can_service_get_udp_to_can_latency(void)
{
    return s_udpToCanLatency;
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

void can_service_reset_stats(void)
{
    for (uint8_t i = 0U; i < CAN_SERVICE_CHANNEL_COUNT; i++)
    {
        can_channel_t *ch = &s_can[i];
        can_service_state_t state = ch->status.state;

        memset(&ch->status, 0, sizeof(ch->status));
        ch->status.state = state;
        can_sync_channel_from_config(ch);
        ch->status.rxQueued = ch->rxQueued;
        ch->status.txQueued = ch->txQueued;
    }
    latency_stat_reset(&s_udpToCanLatency);
    s_lastConfigStatus = CAN_SERVICE_CONFIG_OK;
}

void can_service_tick_1ms(void)
{
    s_ms++;
}
