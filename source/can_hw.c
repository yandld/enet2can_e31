/*
 * can_hw.c - six-channel FlexCAN layer of the E2CF gateway
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Derived from the validated enet2can_e31 can_service.c (MB plumbing, frame
 * conversion, timing calculation, error-state machine) and restructured per
 * design doc 03: interrupt-driven RX (MB banks on all six instances; CAN0's
 * eFIFO is parked, see e2cf_config.h), single active TX MB per channel for
 * strict ordering, per-channel sw_txfifo that realizes the protocol's
 * 16-deep sliding window.
 */
#include "can_hw.h"

#include <string.h>

#include "dbg_log.h"
#include "fsl_clock.h"
#include "fsl_flexcan.h"
#include "gw_prof.h"
#include "gw_time.h"

/* ---------------------------------------------------------------------- */
/* Channel plan                                                            */
/* ---------------------------------------------------------------------- */

#define CAN_TX_MB_IDX 0U /* single active TX MB, always index 0 */
#define CAN_RX_MB_FIRST 1U

/* RX MB CODE values (the SDK enum _flexcan_mb_code_rx is private to
 * fsl_flexcan.c): FULL=0x2, OVERRUN=0x6. */
#define CAN_RX_MB_CODE_FULL    0x2U
#define CAN_RX_MB_CODE_OVERRUN 0x6U
#define CAN_TX_TIMEOUT_MS 100U /* stuck-TX abort (bus stuck dominant etc.) */

typedef struct
{
    CAN_Type *base;
    clock_name_t clk_name;
    IRQn_Type irq_ored;   /* status / error line */
    IRQn_Type irq_mb0;    /* MB 0-31 line (all MBs we use live here) */
    uint8_t rx_mb_count;   /* RX MB bank depth (TX MB is index 0, bank follows) */
} can_plan_t;

static const can_plan_t s_plan[E2CF_NUM_CHANNELS] = {
    { FLEXCAN_0, kCLOCK_Flexcan0Clk, FlexCAN0_0_IRQn, FlexCAN0_1_IRQn, E2CF_RX_MB_CAN0 },
    { FLEXCAN_1, kCLOCK_Flexcan1Clk, FlexCAN1_0_IRQn, FlexCAN1_1_IRQn, E2CF_RX_MB_CAN12 },
    { FLEXCAN_2, kCLOCK_Flexcan2Clk, FlexCAN2_0_IRQn, FlexCAN2_1_IRQn, E2CF_RX_MB_CAN12 },
    { FLEXCAN_3, kCLOCK_Flexcan3Clk, FlexCAN3_0_IRQn, FlexCAN3_1_IRQn, E2CF_RX_MB_CAN345 },
    { FLEXCAN_4, kCLOCK_Flexcan4Clk, FlexCAN4_0_IRQn, FlexCAN4_1_IRQn, E2CF_RX_MB_CAN345 },
    { FLEXCAN_5, kCLOCK_Flexcan5Clk, FlexCAN5_0_IRQn, FlexCAN5_1_IRQn, E2CF_RX_MB_CAN345 },
};

/* sw_txfifo element: the queued send request (window slot). */
typedef struct
{
    uint32_t can_id; /* SocketCAN encoding */
    uint32_t eth_rx_ns; /* gw low32 ns of the request frame's EMAC arrival */
    uint32_t enq_cyc;   /* gw_cycles() at enqueue (gw_prof TX_FIFOWAIT origin) */
    uint8_t len;
    uint8_t flags;   /* E2CF_DATA_FLAG_* */
    uint8_t tag;     /* echo_id, returned in TXC */
    uint8_t data[64];
} can_tx_req_t;

typedef struct
{
    CAN_Type *base;
    const can_plan_t *plan;
    uint8_t index;
    uint32_t clk_freq;
    volatile bool running;
    bool use_fd;
    bool brs_allowed;
    can_hw_bittiming_t bt;          /* raw protocol timing, applied at START */
    flexcan_timing_config_t timing; /* derived register values */

    /* TX path - written from EMAC ISR (submit) and CAN ISR (complete). */
    can_tx_req_t fifo[E2CF_SW_TXFIFO_DEPTH];
    volatile uint8_t fifo_head;
    volatile uint8_t fifo_tail;
    volatile uint8_t fifo_count;
    volatile bool tx_busy;
    uint8_t tx_cur_tag;
    uint32_t tx_cur_eth_rx; /* eth_rx_ns of the frame currently on the MB */
    uint32_t tx_mb_cyc;     /* gw_cycles() at TX MB load (gw_prof TX_BUS origin) */
    uint32_t tx_start_ms;

    /* Error reporting state. */
    uint8_t evt_state; /* E2CF_STATE_* */
    uint16_t rx_ovf_cnt;

    can_hw_stats_t stats;
} can_ch_t;

static can_ch_t s_ch[E2CF_NUM_CHANNELS];
static volatile bool s_tx_gate; /* true = safe state, no new TX MB loads */
static volatile uint32_t s_ms; /* coarse ms from main SysTick, for timeouts */

/* chan_drain_mb_bank() sizes its frame scratch by the deepest bank. */
#if (E2CF_RX_MB_CAN0 > E2CF_RX_MB_CAN12) || (E2CF_RX_MB_CAN345 > E2CF_RX_MB_CAN12)
#error "RX MB bank depths must fit chan_drain_mb_bank frames[E2CF_RX_MB_CAN12]"
#endif

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static const uint8_t s_dlc2len[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

/* Smallest CAN FD DLC code that fits the given payload byte count. */
static uint8_t len_to_dlc(uint8_t len)
{
    if (len <= 8U)
    {
        return len;
    }
    for (uint8_t dlc = 9U; dlc <= 15U; dlc++)
    {
        if (len <= s_dlc2len[dlc])
        {
            return dlc;
        }
    }
    return 15U;
}

/* FlexCAN MB data words pack bytes big-endian within each word. */
static void words_to_bytes(uint8_t *bytes, const uint32_t *words, uint8_t n)
{
    for (uint8_t i = 0U; i < n; i++)
    {
        bytes[i] = (uint8_t)((words[i >> 2] >> (24U - (8U * (i & 3U)))) & 0xFFU);
    }
}

/*
 * Pack payload bytes big-endian into message-buffer words (inverse of
 * words_to_bytes).
 */
static void bytes_to_words(uint32_t *words, const uint8_t *bytes, uint8_t n)
{
    uint8_t clear_len = (uint8_t)((n + 3U) & ~3U);

    if (clear_len != 0U)
    {
        memset(words, 0, clear_len);
    }
    for (uint8_t i = 0U; i < n; i++)
    {
        words[i >> 2] |= ((uint32_t)bytes[i] << (24U - (8U * (i & 3U))));
    }
}

/*
 * Convert protocol raw timing (tq counts) into SDK register values.
 * SDK convention: nominal fields each hold (tq - 1), so total nominal tq =
 * (preDivider+1) * (propSeg + phaseSeg1 + phaseSeg2 + 4) including the sync
 * seg; FD PROPSEG holds tq directly (no -1), so data total uses +3.
 *
 * Bounds are the ENCBT/EDCBT register field widths this device actually uses
 * (NOT the legacy CTRL1/CBT widths): NTSEG1 = nom_tseg1-1 (8 bits, u8 always
 * fits), NTSEG2 = nom_tseg2-1 (7 bits -> tseg2 <= 128); DTSEG1 = dat_tseg1-1
 * (5 bits -> tseg1 <= 32), DTSEG2 = dat_tseg2-1 (4 bits -> tseg2 <= 16).
 * EPRS prescaler fields are 10 bits (brp <= 1024).
 */
static bool timing_from_raw(const can_hw_bittiming_t *bt, flexcan_timing_config_t *t)
{
    uint8_t ps1, prop;

    memset(t, 0, sizeof(*t));

    if ((bt->nom_brp == 0U) || (bt->nom_brp > 1024U) ||
        (bt->nom_tseg1 < 2U) || (bt->nom_tseg2 < 2U) || (bt->nom_tseg2 > 128U) ||
        (bt->nom_sjw == 0U) || (bt->nom_sjw > bt->nom_tseg2))
    {
        return false;
    }
    ps1 = bt->nom_tseg1 / 2U;
    prop = (uint8_t)(bt->nom_tseg1 - ps1);
    t->preDivider = (uint16_t)(bt->nom_brp - 1U);
    t->propSeg = (uint8_t)(prop - 1U);
    t->phaseSeg1 = (uint8_t)(ps1 - 1U);
    t->phaseSeg2 = (uint8_t)(bt->nom_tseg2 - 1U);
    t->rJumpwidth = (uint8_t)(bt->nom_sjw - 1U);

    if ((bt->mode_flags & E2CF_MODE_FD) != 0U)
    {
        if ((bt->dat_brp == 0U) || (bt->dat_tseg1 < 2U) || (bt->dat_tseg2 < 2U) ||
            (bt->dat_tseg1 > 32U) || (bt->dat_tseg2 > 16U) ||
            (bt->dat_sjw == 0U) || (bt->dat_sjw > bt->dat_tseg2))
        {
            return false;
        }
        ps1 = bt->dat_tseg1 / 2U;
        prop = (uint8_t)(bt->dat_tseg1 - ps1);
        t->fpreDivider = (uint16_t)(bt->dat_brp - 1U);
        t->fpropSeg = prop; /* note: FD PROPSEG holds tq directly (no -1), per RM */
        t->fphaseSeg1 = (uint8_t)(ps1 - 1U);
        t->fphaseSeg2 = (uint8_t)(bt->dat_tseg2 - 1U);
        t->frJumpwidth = (uint8_t)(bt->dat_sjw - 1U);
    }
    return true;
}

/*
 * Total time quanta per bit from SDK register values. Nominal-phase fields
 * hold (tq - 1) -> regs sum + 4 (incl. sync seg); FD PROPSEG holds tq
 * directly -> regs sum + 3. Same formulas as the SDK init quantum math.
 */
static uint32_t nom_total_tq(const flexcan_timing_config_t *t)
{
    return ((uint32_t)t->preDivider + 1U) *
           ((uint32_t)t->propSeg + t->phaseSeg1 + t->phaseSeg2 + 4U);
}

static uint32_t dat_total_tq(const flexcan_timing_config_t *t)
{
    return ((uint32_t)t->fpreDivider + 1U) *
           ((uint32_t)t->fpropSeg + t->fphaseSeg1 + t->fphaseSeg2 + 3U);
}

/*
 * FLEXCAN_Init/FDInit re-derive the prescaler from a bits-per-second hint and
 * guard it with asserts that are LIVE in this build (-DDEBUG): bitRate <= 1M
 * (data <= 8M), tq_freq <= clk, and (tq_freq * 1023) >= clk - the last one
 * computed in uint32 and therefore subject to wrap for tq_freq above ~4.2M.
 * Mirror the EXACT arithmetic (including the wrap) on the rates that
 * chan_apply_and_start will pass, so any combination that would assert-halt
 * the gateway inside the EMAC ISR is rejected with EINVAL at CFG time.
 */
static bool sdk_init_guard_ok(uint32_t clk, const flexcan_timing_config_t *t, uint16_t mode_flags)
{
    uint32_t nom_q = (uint32_t)t->propSeg + t->phaseSeg1 + t->phaseSeg2 + 4U;
    uint32_t nom_bps = clk / nom_total_tq(t);
    uint32_t tq_freq = nom_bps * nom_q;

    /* 1M arbitration-phase ceiling: CAN bit-wise arbitration physics; the
     * SDK asserts it as a literal in FLEXCAN_Init ("bitRate <= 1000000U",
     * fsl_flexcan.c) and exposes no feature macro for it. */
    if ((nom_bps == 0U) || (nom_bps > 1000000U) || (tq_freq > clk) ||
        ((uint32_t)(tq_freq * 1023U) < clk))
    {
        return false;
    }
    if ((mode_flags & E2CF_MODE_FD) != 0U)
    {
        uint32_t dat_q = (uint32_t)t->fpropSeg + t->fphaseSeg1 + t->fphaseSeg2 + 3U;
        uint32_t dat_bps = clk / dat_total_tq(t);
        /* same bitRateFD selection as chan_apply_and_start (no-BRS => nom) */
        uint32_t fd_bps = (dat_bps > nom_bps) ? dat_bps : nom_bps;
        uint32_t ftq_freq = fd_bps * dat_q;

        /* Data-phase ceiling: NXP device characterization for this part
         * (8M on MCXE31B, MCXE31B_features.h) - FLEXCAN_FDInit asserts
         * bitRateFD against this same feature macro, not a project pick. */
        if ((fd_bps > (uint32_t)FSL_FEATURE_FLEXCAN_MAX_CANFD_BITRATE) ||
            (ftq_freq > clk) ||
            ((uint32_t)(ftq_freq * 1023U) < clk))
        {
            return false;
        }
    }
    return true;
}

/* Default timing for autostart: SDK-calculated 1M/8M @ 80 MHz. */
static bool timing_default(can_ch_t *ch)
{
    flexcan_timing_config_t timing;

    memset(&timing, 0, sizeof(timing));
    if (!FLEXCAN_FDCalculateImprovedTimingValues(ch->base, E2CF_DEFAULT_NOM_BITRATE,
                                                 E2CF_DEFAULT_DAT_BITRATE, ch->clk_freq, &timing))
    {
        return false;
    }
    ch->timing = timing;
    ch->bt.mode_flags = E2CF_MODE_FD;
    return true;
}

static void log_achieved_bitrate(const can_ch_t *ch)
{
    uint32_t n_tq = nom_total_tq(&ch->timing);
    uint32_t d_tq = dat_total_tq(&ch->timing);

    LOG_INF(DBG_M_CAN, "CAN%u: PE=%lukHz nominal=%lukbps data=%lukbps fd=%u",
            (unsigned)ch->index, (unsigned long)(ch->clk_freq / 1000U),
            (unsigned long)((n_tq != 0U) ? (ch->clk_freq / n_tq / 1000U) : 0U),
            (unsigned long)((ch->use_fd && (d_tq != 0U)) ? (ch->clk_freq / d_tq / 1000U) : 0U),
            (unsigned)ch->use_fd);
}

/* ---------------------------------------------------------------------- */
/* Channel bring-up                                                        */
/* ---------------------------------------------------------------------- */

static void chan_flush_fifo(can_ch_t *ch, uint8_t txc_status)
{
    /* Report every queued-but-unsent window slot so the Linux side can
     * release it (protocol: every accepted DATA gets exactly one TXC). */
    while (ch->fifo_count > 0U)
    {
        can_tx_req_t *req = &ch->fifo[ch->fifo_tail];

        e2cf_core_can_txc(ch->index, req->tag, txc_status, gw_now_ns_lo32(),
                          req->eth_rx_ns);
        ch->fifo_tail = (uint8_t)((ch->fifo_tail + 1U) % E2CF_SW_TXFIFO_DEPTH);
        ch->fifo_count--;
    }
    if (ch->tx_busy)
    {
        e2cf_core_can_txc(ch->index, ch->tx_cur_tag, txc_status, gw_now_ns_lo32(),
                          ch->tx_cur_eth_rx);
        ch->tx_busy = false;
    }
}

/*
 * Full (re)initialization of one FlexCAN instance from the channel's stored
 * timing and mode: controller init, RX/TX message buffers, interrupts, start.
 */
static uint8_t chan_apply_and_start(can_ch_t *ch)
{
    flexcan_config_t cfg;
    const can_plan_t *plan = ch->plan;
    bool loopback = ((ch->bt.mode_flags & E2CF_MODE_LOOPBACK) != 0U);

    ch->use_fd = ((ch->bt.mode_flags & E2CF_MODE_FD) != 0U);
    ch->brs_allowed = false; /* decided below from the actual phase rates */

    FLEXCAN_GetDefaultConfig(&cfg);
    /* Init/FDInit re-derive the prescaler from bitRate/bitRateFD - they do
     * NOT take it from timingConfig - so pass rates computed from the
     * requested timing (and re-apply the exact registers after init). */
    cfg.bitRate = ch->clk_freq / nom_total_tq(&ch->timing);
    if (ch->use_fd)
    {
        uint32_t dat_bps = ch->clk_freq / dat_total_tq(&ch->timing);

        ch->brs_allowed = (dat_bps > cfg.bitRate);
        /* FDInit asserts (nom < dat && brs) || (nom == dat && !brs). */
        cfg.bitRateFD = ch->brs_allowed ? dat_bps : cfg.bitRate;
    }
    cfg.maxMbNum = (uint8_t)(CAN_RX_MB_FIRST + plan->rx_mb_count);
    cfg.enableLoopBack = loopback;
    cfg.disableSelfReception = loopback ? false : true;
    cfg.enableListenOnlyMode = ((ch->bt.mode_flags & E2CF_MODE_LISTENONLY) != 0U);
    cfg.enableIndividMask = true; /* MCR[IRMQ]: the RX MB bank is a reception queue */
    cfg.timingConfig = ch->timing;

    if (ch->use_fd)
    {
        FLEXCAN_FDInit(ch->base, &cfg, ch->clk_freq, kFLEXCAN_64BperMB, ch->brs_allowed);
        /* Undo the SDK's prescaler re-derivation: re-apply the requested
         * timing exactly (the SDK doc blesses this - "SetTimingConfig
         * overrides the bit rate set in Init"). FDInit computed ETDCOFF from
         * the requested timingConfig already, so TDC stays consistent. */
        FLEXCAN_SetTimingConfig(ch->base, &ch->timing);
        FLEXCAN_SetFDTimingConfig(ch->base, &ch->timing);
        /* TDC: the SDK sets a measured TDC up for the real-bus case. Loopback
         * requires TDC off (RM; validated on hardware in enet2can_e31). An
         * explicit CFG tdc_off overrides the SDK default. */
        if (loopback)
        {
            (void)FLEXCAN_EnterFreezeMode(ch->base);
            ch->base->ETDC = 0U;
            ch->base->FDCTRL &= ~(CAN_FDCTRL_TDCEN_MASK | CAN_FDCTRL_TDCOFF_MASK);
            (void)FLEXCAN_ExitFreezeMode(ch->base);
        }
        else if (ch->bt.tdc_off != 0U)
        {
            (void)FLEXCAN_EnterFreezeMode(ch->base);
            ch->base->ETDC = CAN_ETDC_ETDCEN_MASK | CAN_ETDC_ETDCOFF(ch->bt.tdc_off);
            (void)FLEXCAN_ExitFreezeMode(ch->base);
        }
    }
    else
    {
        FLEXCAN_Init(ch->base, &cfg, ch->clk_freq);
        /* Same prescaler re-apply as the FD branch. */
        FLEXCAN_SetTimingConfig(ch->base, &ch->timing);
    }

    /* TX message buffer (single active MB = strict ordering). */
    if (ch->use_fd)
    {
        FLEXCAN_SetFDTxMbConfig(ch->base, CAN_TX_MB_IDX, true);
    }
    else
    {
        FLEXCAN_SetTxMbConfig(ch->base, CAN_TX_MB_IDX, true);
    }

    /* RX MB bank (all six instances): same accept-all id/mask on every MB;
     * with IRMQ the bank acts as a reception queue (next free MB takes the
     * frame). CAN0's Enhanced RX FIFO is deliberately not used - see the
     * resource-plan note in e2cf_config.h. */
    {
        flexcan_rx_mb_config_t mb_cfg;

        mb_cfg.format = kFLEXCAN_FrameFormatStandard;
        mb_cfg.type = kFLEXCAN_FrameTypeData;
        mb_cfg.id = FLEXCAN_ID_STD(0U);

        for (uint8_t i = 0U; i < plan->rx_mb_count; i++)
        {
            uint8_t mb_idx = (uint8_t)(CAN_RX_MB_FIRST + i);

            FLEXCAN_SetRxIndividualMask(ch->base, mb_idx, 0U); /* accept all */
            if (ch->use_fd)
            {
                FLEXCAN_SetFDRxMbConfig(ch->base, mb_idx, &mb_cfg, true);
            }
            else
            {
                FLEXCAN_SetRxMbConfig(ch->base, mb_idx, &mb_cfg, true);
            }
        }
    }

    /* MB interrupts: TX MB + RX bank (all MBs are < 32 -> one IRQ line). */
    {
        uint64_t mb_mask = (uint64_t)1U << CAN_TX_MB_IDX;

        for (uint8_t i = 0U; i < plan->rx_mb_count; i++)
        {
            mb_mask |= (uint64_t)1U << (CAN_RX_MB_FIRST + i);
        }
        FLEXCAN_EnableMbInterrupts(ch->base, mb_mask);
    }

    ch->tx_busy = false;
    ch->fifo_head = 0U;
    ch->fifo_tail = 0U;
    ch->fifo_count = 0U;
    ch->evt_state = E2CF_STATE_ERROR_ACTIVE;
    ch->running = true;

    log_achieved_bitrate(ch);
    return E2CF_CFG_OK;
}

/*
 * Initialize every active channel: PE clocks, default bit timing, NVIC setup
 * and the optional autostart.
 */
bool can_hw_init(void)
{
    memset(s_ch, 0, sizeof(s_ch));
    s_tx_gate = false;

    for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
    {
        can_ch_t *ch = &s_ch[i];

        ch->base = s_plan[i].base;
        ch->plan = &s_plan[i];
        ch->index = i;
        ch->clk_freq = CLOCK_GetFreq(s_plan[i].clk_name);

        if ((E2CF_ACTIVE_CHAN_MASK & (1UL << i)) == 0U)
        {
            continue;
        }

        if (!timing_default(ch))
        {
            LOG_ERR(DBG_M_CAN, "CAN%u: no exact 1M/8M timing at PE=%luHz", i,
                    (unsigned long)ch->clk_freq);
            return false;
        }

        /* NVIC: CAN RX/TXC at the highest data-plane priority (doc 03 §3.2);
         * the ORed status/error line is served too (defensive; no error
         * interrupts are enabled so it stays quiet). */
        NVIC_SetPriority(s_plan[i].irq_ored, E2CF_IRQPRIO_CAN_RX);
        NVIC_SetPriority(s_plan[i].irq_mb0, E2CF_IRQPRIO_CAN_RX);
        (void)EnableIRQ(s_plan[i].irq_ored);
        (void)EnableIRQ(s_plan[i].irq_mb0);

#if E2CF_AUTOSTART_CHANNELS
#if E2CF_AUTOSTART_LOOPBACK
        ch->bt.mode_flags |= E2CF_MODE_LOOPBACK;
#endif
        if (chan_apply_and_start(ch) != E2CF_CFG_OK)
        {
            return false;
        }
#endif
    }

    LOG_INF(DBG_M_CAN, "can_hw: mask=0x%02x autostart=%u", E2CF_ACTIVE_CHAN_MASK,
            (unsigned)E2CF_AUTOSTART_CHANNELS);
    return true;
}

/* ---------------------------------------------------------------------- */
/* CFG operations                                                          */
/* ---------------------------------------------------------------------- */

static bool chan_valid(uint8_t ch_idx)
{
    return (ch_idx < E2CF_NUM_CHANNELS) && ((E2CF_ACTIVE_CHAN_MASK & (1UL << ch_idx)) != 0U);
}

uint8_t can_hw_set_bitrate(uint8_t ch_idx, const can_hw_bittiming_t *bt)
{
    can_ch_t *ch;
    flexcan_timing_config_t timing;

    if (!chan_valid(ch_idx) || (bt == NULL))
    {
        return E2CF_CFG_EINVAL;
    }
    ch = &s_ch[ch_idx];
    if (ch->running)
    {
        return E2CF_CFG_EBUSY; /* STOP first (spec §4.6) */
    }
    if ((bt->mode_flags & E2CF_MODE_ONESHOT) != 0U)
    {
        return E2CF_CFG_ENOTSUP; /* v1: no one-shot support (FlexCAN AERR path TBD) */
    }
    if ((bt->mode_flags & E2CF_MODE_NONISO) != 0U)
    {
        return E2CF_CFG_ENOTSUP; /* v1: ISO FD only (CTRL2[ISOCANFDEN] stays set) */
    }
    if (!timing_from_raw(bt, &timing))
    {
        return E2CF_CFG_EINVAL;
    }
    if (!sdk_init_guard_ok(ch->clk_freq, &timing, bt->mode_flags))
    {
        return E2CF_CFG_EINVAL; /* would trip a live SDK assert at START */
    }
    ch->bt = *bt;
    ch->timing = timing;
    LOG_INF(DBG_M_CFG, "CAN%u: bitrate nom_brp=%u tseg=%u/%u dat_brp=%u tseg=%u/%u mode=0x%x",
            ch_idx, bt->nom_brp, bt->nom_tseg1, bt->nom_tseg2, bt->dat_brp,
            bt->dat_tseg1, bt->dat_tseg2, bt->mode_flags);
    return E2CF_CFG_OK;
}

uint8_t can_hw_set_filter(uint8_t ch_idx, uint8_t bank, uint32_t can_id, uint32_t can_mask)
{
    /* v1 keeps hardware accept-all on every channel; per-bank hardware filter
     * programming (RXIMR / eFIFO table rewrite needs freeze mode) is Phase 2.
     * Accepting and ignoring would silently violate the contract -> ENOTSUP
     * unless it is the default accept-all request. */
    (void)bank;
    if (!chan_valid(ch_idx))
    {
        return E2CF_CFG_EINVAL;
    }
    if (can_mask == 0U)
    {
        return E2CF_CFG_OK; /* accept-all = hardware default */
    }
    LOG_WRN(DBG_M_CFG, "CAN%u: SET_FILTER id=0x%lx mask=0x%lx not supported in v1",
            ch_idx, (unsigned long)can_id, (unsigned long)can_mask);
    return E2CF_CFG_ENOTSUP;
}

uint8_t can_hw_start(uint8_t ch_idx)
{
    if (!chan_valid(ch_idx))
    {
        return E2CF_CFG_EINVAL;
    }
    if (s_ch[ch_idx].running)
    {
        return E2CF_CFG_OK; /* idempotent */
    }
    return chan_apply_and_start(&s_ch[ch_idx]);
}

uint8_t can_hw_stop(uint8_t ch_idx)
{
    can_ch_t *ch;
    uint32_t primask;

    if (!chan_valid(ch_idx))
    {
        return E2CF_CFG_EINVAL;
    }
    ch = &s_ch[ch_idx];
    if (!ch->running)
    {
        return E2CF_CFG_OK;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    ch->running = false;
    chan_flush_fifo(ch, E2CF_TXC_CHAN_STOPPED);
    __set_PRIMASK(primask);

    /* Module reset aborts any pending transmission and clears all flags. */
    FLEXCAN_Deinit(ch->base);
    LOG_INF(DBG_M_CFG, "CAN%u: stopped", ch_idx);
    return E2CF_CFG_OK;
}

uint8_t can_hw_reset(uint8_t ch_idx)
{
    uint8_t st = can_hw_stop(ch_idx);

    if (st != E2CF_CFG_OK)
    {
        return st;
    }
    return can_hw_clear_stats(ch_idx);
}

uint8_t can_hw_clear_stats(uint8_t ch_idx)
{
    if (!chan_valid(ch_idx))
    {
        return E2CF_CFG_EINVAL;
    }
    /* Counters only - controller, FIFO and running state untouched. Races
     * with ISR increments are per-counter atomic (u32 stores), worst case
     * one lost increment. */
    memset(&s_ch[ch_idx].stats, 0, sizeof(s_ch[ch_idx].stats));
    s_ch[ch_idx].rx_ovf_cnt = 0U;
    return E2CF_CFG_OK;
}

bool can_hw_is_running(uint8_t ch_idx)
{
    return chan_valid(ch_idx) && s_ch[ch_idx].running;
}

void can_hw_get_bitrate(uint8_t ch_idx, can_hw_bittiming_t *out)
{
    if (chan_valid(ch_idx) && (out != NULL))
    {
        *out = s_ch[ch_idx].bt;
    }
}

static void chan_tx_kick(can_ch_t *ch);

void can_hw_set_tx_gate(bool gated)
{
    s_tx_gate = gated;
    if (!gated)
    {
        /* Releasing the gate must restart channels whose sw_txfifo backed
         * up while gated: nothing else kicks until the NEXT submit on the
         * same channel, so queued frames (and their pending TXCs) would
         * otherwise stall until unrelated traffic arrives. */
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
        {
            if (s_ch[i].running)
            {
                chan_tx_kick(&s_ch[i]);
            }
        }
        __set_PRIMASK(primask);
    }
}

/* ---------------------------------------------------------------------- */
/* TX path                                                                 */
/* ---------------------------------------------------------------------- */

/* Load the next queued request into the (idle) TX MB. IRQ-masked callers. */
static void chan_tx_kick(can_ch_t *ch)
{
    can_tx_req_t *req;
    flexcan_fd_frame_t fd;
    flexcan_frame_t classic;
    bool ext;
    status_t st;

    if (ch->tx_busy || (ch->fifo_count == 0U) || !ch->running || s_tx_gate)
    {
        return;
    }

    req = &ch->fifo[ch->fifo_tail];
    ch->fifo_tail = (uint8_t)((ch->fifo_tail + 1U) % E2CF_SW_TXFIFO_DEPTH);
    ch->fifo_count--;

    ext = ((req->can_id & E2CF_CAN_EFF_FLAG) != 0U);

    if (ch->use_fd && ((req->flags & E2CF_DATA_FLAG_FDF) != 0U))
    {
        memset(&fd, 0, sizeof(fd));
        fd.id = ext ? FLEXCAN_ID_EXT(req->can_id & E2CF_CAN_EFF_MASK)
                    : FLEXCAN_ID_STD(req->can_id & E2CF_CAN_SFF_MASK);
        fd.format = ext ? (uint8_t)kFLEXCAN_FrameFormatExtend : (uint8_t)kFLEXCAN_FrameFormatStandard;
        fd.type = (uint8_t)kFLEXCAN_FrameTypeData;
        fd.length = len_to_dlc(req->len);
        fd.brs = (((req->flags & E2CF_DATA_FLAG_BRS) != 0U) && ch->brs_allowed) ? 1U : 0U;
        fd.edl = 1U;
        bytes_to_words(fd.dataWord, req->data, req->len);
        st = FLEXCAN_WriteFDTxMb(ch->base, CAN_TX_MB_IDX, &fd);
    }
    else
    {
        memset(&classic, 0, sizeof(classic));
        classic.id = ext ? FLEXCAN_ID_EXT(req->can_id & E2CF_CAN_EFF_MASK)
                         : FLEXCAN_ID_STD(req->can_id & E2CF_CAN_SFF_MASK);
        classic.format = ext ? (uint8_t)kFLEXCAN_FrameFormatExtend : (uint8_t)kFLEXCAN_FrameFormatStandard;
        classic.type = ((req->can_id & E2CF_CAN_RTR_FLAG) != 0U) ? (uint8_t)kFLEXCAN_FrameTypeRemote
                                                                 : (uint8_t)kFLEXCAN_FrameTypeData;
        classic.length = (req->len > 8U) ? 8U : req->len;
        bytes_to_words(&classic.dataWord0, req->data, classic.length);
        st = FLEXCAN_WriteTxMb(ch->base, CAN_TX_MB_IDX, &classic);
    }

    if (st != kStatus_Success)
    {
        /* Single MB and tx_busy was false: cannot normally happen. Count it
         * and confirm the slot as a controller error so the window drains. */
        ch->stats.tx_rejected++;
        e2cf_core_can_txc(ch->index, req->tag, E2CF_TXC_CTRL_ERROR, gw_now_ns_lo32(),
                          req->eth_rx_ns);
        return;
    }

    ch->tx_busy = true;
    ch->tx_cur_tag = req->tag;
    ch->tx_cur_eth_rx = req->eth_rx_ns;
    ch->tx_start_ms = s_ms;
#if E2CF_PROFILE
    /* Time the wait this request spent in sw_txfifo behind the single active
     * TX MB, and open the bus-transmission interval (closed in
     * chan_tx_complete). Both use one timestamp so they share an origin. */
    ch->tx_mb_cyc = gw_cycles();
    gw_prof_add(PROF_TX_FIFOWAIT, ch->tx_mb_cyc - req->enq_cyc);
#endif
}

uint8_t can_hw_submit_tx(const e2cf_data_rec_t *head, const uint8_t *payload,
                         uint32_t eth_rx_ns)
{
    can_ch_t *ch;
    can_tx_req_t *req;
    uint8_t ch_idx = (uint8_t)(head->chan & 0x07U);
    uint32_t primask;

    if (!chan_valid(ch_idx) || (head->len > 64U))
    {
        return E2CF_TXC_CTRL_ERROR;
    }
    ch = &s_ch[ch_idx];
    if (!ch->running || s_tx_gate)
    {
        ch->stats.tx_rejected++;
        return E2CF_TXC_CHAN_STOPPED;
    }
    if ((head->can_id & E2CF_CAN_RTR_FLAG) != 0U && (head->flags & E2CF_DATA_FLAG_FDF) != 0U)
    {
        ch->stats.tx_rejected++;
        return E2CF_TXC_CTRL_ERROR; /* no remote frames in FD */
    }
    if ((head->flags & E2CF_DATA_FLAG_FDF) != 0U)
    {
        if (!ch->use_fd)
        {
            ch->stats.tx_rejected++;
            return E2CF_TXC_CTRL_ERROR; /* FD record on a classical-only channel */
        }
        if (((head->flags & E2CF_DATA_FLAG_BRS) != 0U) && !ch->brs_allowed)
        {
            ch->stats.tx_rejected++;
            return E2CF_TXC_CTRL_ERROR; /* BRS on a channel without rate switch
                                         * (dbitrate <= bitrate): reject, never
                                         * silently strip the flag */
        }
    }
    else if ((head->len > 8U) || ((head->flags & E2CF_DATA_FLAG_BRS) != 0U))
    {
        ch->stats.tx_rejected++;
        return E2CF_TXC_CTRL_ERROR; /* classical record: len <= 8, no BRS */
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if (ch->fifo_count >= E2CF_SW_TXFIFO_DEPTH)
    {
        /* Window overrun: the Linux side violated flow control (or lost
         * TXCs). Reject newest, report, keep the queue intact. */
        __set_PRIMASK(primask);
        ch->stats.tx_rejected++;
        return E2CF_TXC_QUEUE_OVF;
    }
    req = &ch->fifo[ch->fifo_head];
    req->can_id = head->can_id;
    req->eth_rx_ns = eth_rx_ns;
#if E2CF_PROFILE
    req->enq_cyc = gw_cycles();
#endif
    req->len = head->len;
    req->flags = head->flags;
    req->tag = head->tag;
    if (head->len != 0U)
    {
        memcpy(req->data, payload, head->len);
    }
    ch->fifo_head = (uint8_t)((ch->fifo_head + 1U) % E2CF_SW_TXFIFO_DEPTH);
    ch->fifo_count++;
    if (ch->fifo_count > ch->stats.fifo_hwm)
    {
        ch->stats.fifo_hwm = ch->fifo_count;
    }
    gw_prof_add(PROF_TXFIFO_DEPTH, ch->fifo_count);
    chan_tx_kick(ch);
    __set_PRIMASK(primask);

    return E2CF_TXC_OK;
}

/* ---------------------------------------------------------------------- */
/* RX path (IRQ context, priority 1)                                       */
/* ---------------------------------------------------------------------- */

/* Deliver one received hardware frame upward as SocketCAN-encoded fields. */
static void deliver_rx(can_ch_t *ch, const flexcan_fd_frame_t *f, bool is_fd)
{
    uint32_t can_id;
    uint8_t flags = 0U;
    uint8_t len;
    uint8_t bytes[64];

    if (f->format == (uint8_t)kFLEXCAN_FrameFormatExtend)
    {
        can_id = (f->id & E2CF_CAN_EFF_MASK) | E2CF_CAN_EFF_FLAG;
    }
    else
    {
        can_id = (f->id >> CAN_ID_STD_SHIFT) & E2CF_CAN_SFF_MASK;
    }

    if (is_fd && (f->edl != 0U))
    {
        flags |= E2CF_DATA_FLAG_FDF;
        if (f->brs != 0U)
        {
            flags |= E2CF_DATA_FLAG_BRS;
        }
        if (f->esi != 0U)
        {
            flags |= E2CF_DATA_FLAG_ESI;
        }
        len = s_dlc2len[f->length & 0x0FU];
    }
    else
    {
        if (f->type == (uint8_t)kFLEXCAN_FrameTypeRemote)
        {
            can_id |= E2CF_CAN_RTR_FLAG;
        }
        len = (f->length > 8U) ? 8U : (uint8_t)f->length;
    }

    if ((can_id & E2CF_CAN_RTR_FLAG) != 0U)
    {
        /* Remote frame: len = requested DLC, no data section on the bus -
         * the MB data area is stale memory, ship zeros (spec §4.3). */
        memset(bytes, 0, len);
    }
    else
    {
        words_to_bytes(bytes, f->dataWord, len);
    }
    /* One-shot INFO marker proves the controller-level RX path of this
     * channel on a default-level console; per-frame detail is DEBUG. */
    if (ch->stats.rx_frames == 0U)
    {
        LOG_INF(DBG_M_CAN, "CAN%u first RX: id=0x%08lx len=%u fl=0x%02x",
                ch->index, (unsigned long)can_id, len, flags);
    }
    LOG_DBG(DBG_M_CAN, "CAN%u rx id=0x%08lx len=%u fl=0x%02x",
            ch->index, (unsigned long)can_id, len, flags);
    ch->stats.rx_frames++;
    e2cf_core_can_rx(ch->index, can_id, len, flags, bytes, gw_now_ns_lo32());
}

/*
 * All channels: scan the whole RX MB bank, read every pending MB (SDK read does the
 * CS-lock / TIMER-unlock protocol), then sort by the 16-bit free-running
 * capture timestamp before delivery - the hardware fills "first free matching
 * MB" so MB index order is NOT arrival order (doc 03 §3.3).
 */
static void chan_drain_mb_bank(can_ch_t *ch, uint32_t pending_mask)
{
    flexcan_fd_frame_t frames[E2CF_RX_MB_CAN12]; /* max bank size */
    uint8_t order[E2CF_RX_MB_CAN12];
    uint8_t n = 0U;

    for (uint8_t i = 0U; i < ch->plan->rx_mb_count; i++)
    {
        uint8_t mb_idx = (uint8_t)(CAN_RX_MB_FIRST + i);
        status_t st;

        if ((pending_mask & (1UL << mb_idx)) == 0U)
        {
            continue;
        }
        /* W1C the MB flag BEFORE consuming the MB: a frame that moves in
         * right after the SDK read's TIMER unlock then re-asserts the flag
         * and is picked up by the next ISR pass instead of being stranded
         * FULL with its interrupt flag already cleared. */
        FLEXCAN_ClearMbStatusFlags(ch->base, (uint64_t)1U << mb_idx);
        if (ch->use_fd)
        {
            /* FLEXCAN_ReadFDRxMb() decodes neither RTR nor ESI from the CS
             * word (type is hardcoded Data, esi is left untouched), so peek
             * CS first. On a settled MB (FULL/OVERRUN) this read also takes
             * the MB lock that the SDK read then holds until its TIMER
             * unlock, so both see the same frame. */
            uint32_t cs = (&ch->base->MB[0].CS)[FLEXCAN_GetFDMailboxOffset(ch->base, mb_idx)];
            uint8_t cs_code = (uint8_t)((cs & CAN_CS_CODE_MASK) >> CAN_CS_CODE_SHIFT);

            memset(&frames[n], 0, sizeof(frames[n]));
            st = FLEXCAN_ReadFDRxMb(ch->base, mb_idx, &frames[n]);
            /* Trust the peeked bits only if the peek saw a settled MB: a
             * BUSY peek takes no lock and may belong to a different frame
             * than the one the SDK read returns (overrun move-in race).
             * Fallback is esi=0/type=Data from the memset - degraded
             * attribution, never torn payload. */
            if ((cs_code == CAN_RX_MB_CODE_FULL) || (cs_code == CAN_RX_MB_CODE_OVERRUN))
            {
                frames[n].esi = ((cs & CAN_CS_ESI_MASK) != 0U) ? 1U : 0U;
                if (((cs & CAN_CS_EDL_MASK) == 0U) && ((cs & CAN_CS_RTR_MASK) != 0U))
                {
                    frames[n].type = (uint8_t)kFLEXCAN_FrameTypeRemote;
                }
            }
        }
        else
        {
            /* Classic MB layout is the FD layout's first words; read via the
             * classic API into a stack frame then widen. */
            flexcan_frame_t c;

            st = FLEXCAN_ReadRxMb(ch->base, mb_idx, &c);
            if ((st == kStatus_Success) || (st == kStatus_FLEXCAN_RxOverflow))
            {
                memset(&frames[n], 0, sizeof(frames[n]));
                frames[n].timestamp = c.timestamp;
                frames[n].length = c.length;
                frames[n].type = c.type;
                frames[n].format = c.format;
                frames[n].id = c.id;
                frames[n].dataWord[0] = c.dataWord0;
                frames[n].dataWord[1] = c.dataWord1;
            }
        }

        if (st == kStatus_FLEXCAN_RxOverflow)
        {
            ch->stats.rx_ovf++;
            ch->rx_ovf_cnt++;
        }
        else if (st != kStatus_Success)
        {
            continue;
        }
        order[n] = n;
        n++;
    }

    /* Insertion sort by wrap-safe 16-bit timestamp delta (n <= 8). */
    for (uint8_t i = 1U; i < n; i++)
    {
        uint8_t key = order[i];
        int8_t j = (int8_t)(i - 1U);

        while ((j >= 0) &&
               ((int16_t)((uint16_t)frames[order[j]].timestamp -
                          (uint16_t)frames[key].timestamp) > 0))
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    for (uint8_t i = 0U; i < n; i++)
    {
        deliver_rx(ch, &frames[order[i]], ch->use_fd);
    }
}

/* TX-complete handling shared by all channels. */
static void chan_tx_complete(can_ch_t *ch)
{
#if E2CF_PROFILE
    uint32_t gap_t0 = gw_cycles(); /* frame just completed: reload starts now */
#endif
    ch->stats.tx_frames++;
    if (ch->tx_busy)
    {
#if E2CF_PROFILE
        /* Close the bus-transmission interval opened at TX MB load: this is the
         * physical CAN frame time (~111 us for 64B FD at 1M/8M). */
        gw_prof_add(PROF_TX_BUS, gap_t0 - ch->tx_mb_cyc);
#endif
        ch->tx_busy = false;
        e2cf_core_can_txc(ch->index, ch->tx_cur_tag, E2CF_TXC_OK, gw_now_ns_lo32(),
                          ch->tx_cur_eth_rx);
    }
    chan_tx_kick(ch);
#if E2CF_PROFILE
    /* If a queued frame was just loaded, measure the bus reload dead-time: this
     * completion -> the next frame's MB write (TXC append + dequeue + format +
     * WriteFDTxMb). This is the controllable inter-frame idle that keeps the
     * back-to-back rate below the bus theory; the sub-us IRQ-entry latency
     * before gap_t0 is not included. */
    if (ch->tx_busy)
    {
        gw_prof_add(PROF_TX_GAP, ch->tx_mb_cyc - gap_t0);
    }
#endif
}

/* Common IRQ body: MB flags (TX + RX bank). */
static void can_irq(uint8_t ch_idx)
{
    can_ch_t *ch = &s_ch[ch_idx];
    uint64_t mb_flags;
#if E2CF_PROFILE
    uint32_t isr_t0 = gw_cycles();
#endif

    ch->stats.irq_count++;
    if (!ch->running)
    {
        /* Stray flags during STOP: clear everything and leave. */
        FLEXCAN_ClearMbStatusFlags(ch->base, 0xFFFFFFFFFFFFFFFFULL);
        return;
    }

    mb_flags = FLEXCAN_GetMbStatusFlags(ch->base, 0xFFFFFFFFFFFFFFFFULL);

    /* RX first (overflow deadline), then TX-complete, then refill. */
    {
        uint32_t rx_mask = 0U;

        for (uint8_t i = 0U; i < ch->plan->rx_mb_count; i++)
        {
            rx_mask |= (1UL << (CAN_RX_MB_FIRST + i));
        }
        if (((uint32_t)mb_flags & rx_mask) != 0U)
        {
            chan_drain_mb_bank(ch, (uint32_t)mb_flags & rx_mask);
        }
    }

    if ((mb_flags & ((uint64_t)1U << CAN_TX_MB_IDX)) != 0U)
    {
        FLEXCAN_ClearMbStatusFlags(ch->base, (uint64_t)1U << CAN_TX_MB_IDX);
        chan_tx_complete(ch);
    }
#if E2CF_PROFILE
    gw_prof_add(PROF_CAN_ISR, gw_cycles() - isr_t0);
#endif
}

/* Vector handlers: both the ORed-status and the MB0-31 lines funnel into the
 * common body (all MBs we use are < 32; the ORed line never fires with no
 * error interrupts enabled but is served for robustness). */
void FlexCAN0_0_IRQHandler(void) { can_irq(0U); }
void FlexCAN0_1_IRQHandler(void) { can_irq(0U); }
void FlexCAN1_0_IRQHandler(void) { can_irq(1U); }
void FlexCAN1_1_IRQHandler(void) { can_irq(1U); }
void FlexCAN2_0_IRQHandler(void) { can_irq(2U); }
void FlexCAN2_1_IRQHandler(void) { can_irq(2U); }
void FlexCAN3_0_IRQHandler(void) { can_irq(3U); }
void FlexCAN3_1_IRQHandler(void) { can_irq(3U); }
void FlexCAN4_0_IRQHandler(void) { can_irq(4U); }
void FlexCAN4_1_IRQHandler(void) { can_irq(4U); }
void FlexCAN5_0_IRQHandler(void) { can_irq(5U); }
void FlexCAN5_1_IRQHandler(void) { can_irq(5U); }

/* ---------------------------------------------------------------------- */
/* Error supervision (main loop, ~10 ms)                                   */
/* ---------------------------------------------------------------------- */

void can_hw_get_evt(uint8_t ch_idx, e2cf_evt_rec_t *evt)
{
    can_ch_t *ch = &s_ch[ch_idx];
    uint8_t tec = 0U, rec = 0U;

    memset(evt, 0, sizeof(*evt));
    evt->chan = ch_idx;
    if (!chan_valid(ch_idx) || !ch->running)
    {
        evt->state = ch->evt_state;
        return;
    }
    FLEXCAN_GetBusErrCount(ch->base, &tec, &rec);
    evt->state = ch->evt_state;
    evt->tec = tec;
    evt->rec = rec;
    evt->arb_lost_cnt = ch->stats.arb_lost_cnt;
    evt->rx_ovf_cnt = ch->rx_ovf_cnt;
    if (ch->rx_ovf_cnt != 0U)
    {
        evt->err_flags |= E2CF_ERR_RX_OVF;
    }
}

void can_hw_poll_errors(void)
{
    s_ms++; /* called on a 10 ms cadence; 100 ms TX timeout = 10 ticks */

    for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
    {
        can_ch_t *ch = &s_ch[i];
        uint64_t flags;
        uint32_t fault_state;
        uint8_t new_state;

        if (!chan_valid(i) || !ch->running)
        {
            continue;
        }

        flags = FLEXCAN_GetStatusFlags(ch->base);
        fault_state = (uint32_t)((flags & (uint64_t)kFLEXCAN_FaultConfinementFlag) >>
                                CAN_ESR1_FLTCONF_SHIFT);
        if (fault_state >= 2U)
        {
            new_state = E2CF_STATE_BUS_OFF;
        }
        else if (fault_state == 1U)
        {
            new_state = E2CF_STATE_ERROR_PASSIVE;
        }
        else if ((flags & ((uint64_t)kFLEXCAN_TxErrorWarningFlag |
                           (uint64_t)kFLEXCAN_RxErrorWarningFlag)) != 0U)
        {
            new_state = E2CF_STATE_ERROR_WARNING;
        }
        else
        {
            new_state = E2CF_STATE_ERROR_ACTIVE;
        }

        if (new_state != ch->evt_state)
        {
            e2cf_evt_rec_t evt;
            uint8_t old = ch->evt_state;

            ch->evt_state = new_state;
            if (new_state == E2CF_STATE_BUS_OFF)
            {
                ch->stats.bus_off_cnt++;
            }
            can_hw_get_evt(i, &evt);
            LOG_WRN(DBG_M_CAN, "CAN%u: state %u -> %u (tec=%u rec=%u)", i, old,
                    new_state, evt.tec, evt.rec);
            e2cf_core_can_state_change(i, &evt);
        }

        /* Stuck-TX watchdog: bus stuck dominant / no ACK forever. */
        if (ch->tx_busy && ((s_ms - (ch->tx_start_ms)) >= (CAN_TX_TIMEOUT_MS / 10U)))
        {
            uint32_t primask = __get_PRIMASK();

            __disable_irq();
            if (ch->tx_busy) /* re-check under mask: TXC may have just fired */
            {
                /* Reconfiguring the MB to inactive aborts the transmission. */
                if (ch->use_fd)
                {
                    FLEXCAN_SetFDTxMbConfig(ch->base, CAN_TX_MB_IDX, true);
                }
                else
                {
                    FLEXCAN_SetTxMbConfig(ch->base, CAN_TX_MB_IDX, true);
                }
                FLEXCAN_ClearMbStatusFlags(ch->base, (uint64_t)1U << CAN_TX_MB_IDX);
                ch->tx_busy = false;
                ch->stats.tx_timeout++;
                e2cf_core_can_txc(i, ch->tx_cur_tag, E2CF_TXC_CTRL_ERROR, gw_now_ns_lo32(),
                                  ch->tx_cur_eth_rx);
                chan_tx_kick(ch);
            }
            __set_PRIMASK(primask);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Stats                                                                   */
/* ---------------------------------------------------------------------- */

const can_hw_stats_t *can_hw_stats(uint8_t ch_idx)
{
    return (ch_idx < E2CF_NUM_CHANNELS) ? &s_ch[ch_idx].stats : NULL;
}

void can_hw_dump_stats(void)
{
    for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
    {
        can_ch_t *ch = &s_ch[i];

        if (!chan_valid(i))
        {
            continue;
        }
        LOG_INF(DBG_M_STATS,
                "CAN%u%s rx=%lu ovf=%lu tx=%lu rej=%lu tmo=%lu hwm=%lu irq=%lu st=%u",
                i, ch->running ? "" : "(stop)",
                (unsigned long)ch->stats.rx_frames, (unsigned long)ch->stats.rx_ovf,
                (unsigned long)ch->stats.tx_frames, (unsigned long)ch->stats.tx_rejected,
                (unsigned long)ch->stats.tx_timeout, (unsigned long)ch->stats.fifo_hwm,
                (unsigned long)ch->stats.irq_count, ch->evt_state);
    }
}
