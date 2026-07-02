/*
 * eth_raw.c - bare ENET_QOS (EMAC) layer: raw L2 frames, no lwIP
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bring-up recipe (clocks, PHY, descriptor/buffer plumbing) follows the
 * hardware-validated lwIP port of the donor project (enet_ethernetif_qos.c),
 * with the pbuf machinery removed.
 */
#include "eth_raw.h"

#include <string.h>

#include "board.h"
#include "dbg_log.h"
#include "e2cf_config.h"
#include "e2cf_proto.h"
#include "fsl_enet_qos.h"
#include "fsl_phy.h"
#include "fsl_phylan8741.h"
#include "fsl_silicon_id.h"

/* BD alignment: 64B for optimal AHB bursts (donor project value). */
#define ETH_BD_ALIGN 64U
#define ETH_BUFF_ALIGN 64U /* >= cache line (32B on M7) and >= BD alignment */
#define ETH_RXBUFF_SIZE SDK_SIZEALIGN(ENET_QOS_FRAME_MAX_FRAMELEN, ETH_BUFF_ALIGN)
/* A whole frame always fits one RX buffer; 2 entries guard the API contract. */
#define ETH_MAX_BUFFERS_PER_FRAME 2U

typedef uint8_t eth_rx_buffer_t[ETH_RXBUFF_SIZE];

/* Descriptor rings - DMA requires non-cacheable (DTCM region, see scatter). */
AT_NONCACHEABLE_SECTION_ALIGN(static enet_qos_rx_bd_struct_t s_rx_bd[E2CF_ETH_RXBD_NUM],
                              ENET_QOS_BUFF_ALIGNMENT);
AT_NONCACHEABLE_SECTION_ALIGN(static enet_qos_tx_bd_struct_t s_tx_bd[E2CF_ETH_TXBD_NUM],
                              ENET_QOS_BUFF_ALIGNMENT);

/* RX data buffers: cacheable SRAM; the driver invalidates on receive
 * (FSL_ETH_ENABLE_CACHE_CONTROL + rxBuffNeedMaintain). */
SDK_ALIGN(static eth_rx_buffer_t s_rx_buff[E2CF_ETH_RXBUFF_NUM], ETH_BUFF_ALIGN);
static volatile bool s_rx_buff_used[E2CF_ETH_RXBUFF_NUM];

static enet_qos_handle_t s_handle;
static enet_qos_frame_info_t s_tx_dirty[E2CF_ETH_TXBD_NUM];
static uint32_t s_rx_buffer_start_addr[E2CF_ETH_RXBD_NUM];

static phy_handle_t s_phy_handle;
static uint8_t s_mac[6];
static bool s_link_up;
static phy_speed_t s_last_speed = (phy_speed_t)0xA5;
static phy_duplex_t s_last_duplex = (phy_duplex_t)0x5A;

static volatile uint32_t s_tx_submitted;
static volatile uint32_t s_tx_reclaimed;
static struct
{
    uint32_t rx_frames;
    uint32_t rx_errors;
    uint32_t rx_drops; /* pool exhausted / driver drop */
    uint32_t tx_frames;
    uint32_t tx_busy;
} s_stats;

extern phy_lan8741_resource_t g_phy_resource; /* board/hardware_init.c */

/* ---------------------------------------------------------------------- */
/* RX buffer pool (driver alloc/free callbacks)                            */
/* ---------------------------------------------------------------------- */

static void *eth_rx_alloc(ENET_QOS_Type *base, void *userData, uint8_t channel)
{
    uint32_t primask = __get_PRIMASK();
    void *buffer = NULL;

    (void)base;
    (void)userData;
    (void)channel;

    __disable_irq();
    for (uint32_t i = 0U; i < E2CF_ETH_RXBUFF_NUM; i++)
    {
        if (!s_rx_buff_used[i])
        {
            s_rx_buff_used[i] = true;
            buffer = &s_rx_buff[i];
            break;
        }
    }
    __set_PRIMASK(primask);

    if (buffer == NULL)
    {
        s_stats.rx_drops++;
    }
    return buffer;
}

static void eth_rx_free(ENET_QOS_Type *base, void *buffer, void *userData, uint8_t channel)
{
    int32_t idx = (int32_t)(((eth_rx_buffer_t *)buffer) - &s_rx_buff[0]);

    (void)base;
    (void)userData;
    (void)channel;

    if ((idx >= 0) && (idx < (int32_t)E2CF_ETH_RXBUFF_NUM))
    {
        s_rx_buff_used[idx] = false;
    }
}

/* ---------------------------------------------------------------------- */
/* RX dispatch + TX reclaim (EMAC IRQ context)                             */
/* ---------------------------------------------------------------------- */

/*
 * Drain all pending RX frames from the EQOS handle, upcalling each one to
 * e2cf_core and returning the buffer to the pool right after parsing.
 */
static void eth_drain_rx(void)
{
    enet_qos_buffer_struct_t buffers[ETH_MAX_BUFFERS_PER_FRAME];
    enet_qos_rx_frame_struct_t rx_frame;
    status_t status;

    for (;;)
    {
        memset(buffers, 0, sizeof(buffers));
        memset(&rx_frame, 0, sizeof(rx_frame));
        rx_frame.rxBuffArray = &buffers[0];

        status = ENET_QOS_GetRxFrame(EMAC, &s_handle, &rx_frame, 0U);
        if (status == kStatus_Success)
        {
            s_stats.rx_frames++;
            /* E2CF frames always fit one buffer; a chained frame is oversized
             * for this protocol -> parse the first buffer only. */
            e2cf_core_eth_frame((const uint8_t *)buffers[0].buffer,
                                (buffers[0].length < rx_frame.totLen) ? buffers[0].length
                                                                     : rx_frame.totLen);
            for (uint32_t i = 0U; i < ETH_MAX_BUFFERS_PER_FRAME; i++)
            {
                if (buffers[i].buffer != NULL)
                {
                    eth_rx_free(EMAC, buffers[i].buffer, NULL, 0U);
                }
            }
        }
        else if (status == kStatus_ENET_QOS_RxFrameError)
        {
            s_stats.rx_errors++;
        }
        else if (status == kStatus_ENET_QOS_RxFrameDrop)
        {
            s_stats.rx_drops++;
        }
        else /* kStatus_ENET_QOS_RxFrameEmpty */
        {
            break;
        }
    }
}

/*
 * EQOS driver event callback (EMAC IRQ context): drains RX and counts
 * reclaimed TX descriptors.
 */
static void eth_callback(ENET_QOS_Type *base, enet_qos_handle_t *handle,
                         enet_qos_event_t event, uint8_t channel, void *param)
{
    (void)base;
    (void)handle;
    (void)channel;
    (void)param;

    switch (event)
    {
        case kENET_QOS_RxIntEvent:
            eth_drain_rx();
            break;
        case kENET_QOS_TxIntEvent:
            s_tx_reclaimed++;
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------------------- */
/* Init                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Initialize the LAN8741 PHY with autonegotiation through the MDIO resource.
 */
static bool eth_phy_init(void)
{
    phy_config_t phy_config = {
        .phyAddr = BOARD_EMAC_PHY_ADDRESS,
        .ops = &phylan8741_ops,
        .resource = &g_phy_resource,
        .autoNeg = true,
    };

    if (PHY_Init(&s_phy_handle, &phy_config) != kStatus_Success)
    {
        LOG_ERR(DBG_M_ETH, "PHY init failed");
        return false;
    }
    return true;
}

/*
 * Full EMAC bring-up: PHY, MAC address, descriptor rings, interrupt, HB
 * multicast filter and controller start.
 */
bool eth_raw_init(void)
{
    enet_qos_config_t config;
    enet_qos_buffer_config_t buff_cfg;
    static uint8_t hb_mcast[6] = E2CF_HB_MCAST_DA; /* SDK API wants non-const */
    status_t status;

    if (!eth_phy_init())
    {
        return false;
    }

    (void)SILICONID_ConvertToMacAddr(&s_mac);

    memset(&buff_cfg, 0, sizeof(buff_cfg));
    buff_cfg.rxRingLen = E2CF_ETH_RXBD_NUM;
    buff_cfg.txRingLen = E2CF_ETH_TXBD_NUM;
    buff_cfg.txDescStartAddrAlign = &s_tx_bd[0];
    buff_cfg.txDescTailAddrAlign = &s_tx_bd[0];
    buff_cfg.txDirtyStartAddr = &s_tx_dirty[0];
    buff_cfg.rxDescStartAddrAlign = &s_rx_bd[0];
    buff_cfg.rxDescTailAddrAlign = &s_rx_bd[E2CF_ETH_RXBD_NUM];
    buff_cfg.rxBufferStartAddr = s_rx_buffer_start_addr;
    buff_cfg.rxBuffSizeAlign = sizeof(eth_rx_buffer_t);
    buff_cfg.rxBuffNeedMaintain = true; /* RX buffers are in cacheable SRAM */

    ENET_QOS_GetDefaultConfig(&config);
    config.miiMode = kENET_QOS_RmiiMode;
    config.csrClock_Hz = CLOCK_GetAipsPlatClkFreq();
    config.specialControl = kENET_QOS_StoreAndForward;
    config.rxBuffAlloc = eth_rx_alloc;
    config.rxBuffFree = eth_rx_free;

    status = ENET_QOS_Init(EMAC, &config, s_mac, 1U, config.csrClock_Hz);
    if (status != kStatus_Success)
    {
        LOG_ERR(DBG_M_ETH, "ENET_QOS_Init failed (%ld)", (long)status);
        return false;
    }

    ENET_QOS_EnableInterrupts(EMAC, (uint32_t)kENET_QOS_DmaTx | (uint32_t)kENET_QOS_DmaRx);
    ENET_QOS_CreateHandler(EMAC, &s_handle, &config, &buff_cfg, eth_callback, NULL);

    status = ENET_QOS_DescriptorInit(EMAC, &config, &buff_cfg);
    if (status != kStatus_Success)
    {
        LOG_ERR(DBG_M_ETH, "ENET_QOS_DescriptorInit failed (%ld)", (long)status);
        return false;
    }

    status = ENET_QOS_RxBufferAllocAll(EMAC, &s_handle);
    if (status != kStatus_Success)
    {
        LOG_ERR(DBG_M_ETH, "ENET_QOS_RxBufferAllocAll failed (%ld)", (long)status);
        return false;
    }

    /* HB discovery multicast (spec §2.3). Unicast own-MAC filtering is the
     * MAC default; E2CF data frames arrive unicast once the peer learns us. */
    ENET_QOS_AddMulticastGroup(EMAC, hb_mcast);

    NVIC_SetPriority(EMAC_0_IRQn, E2CF_IRQPRIO_EMAC);
    (void)EnableIRQ(EMAC_0_IRQn);

    ENET_QOS_StartRxTx(EMAC, 1U, 1U);

    LOG_INF(DBG_M_ETH, "EMAC up, MAC %02x:%02x:%02x:%02x:%02x:%02x",
            s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Link supervision                                                        */
/* ---------------------------------------------------------------------- */

void eth_raw_poll_link(void)
{
    bool link = false;

    if (PHY_GetLinkStatus(&s_phy_handle, &link) != kStatus_Success)
    {
        return;
    }

    if (link && !s_link_up)
    {
        phy_speed_t speed;
        phy_duplex_t duplex;

        if (PHY_GetLinkSpeedDuplex(&s_phy_handle, &speed, &duplex) == kStatus_Success)
        {
            if ((speed != s_last_speed) || (duplex != s_last_duplex))
            {
                enet_qos_mii_speed_t mii_speed =
                    (speed == kPHY_Speed1000M) ? kENET_QOS_MiiSpeed1000M :
                    (speed == kPHY_Speed100M) ? kENET_QOS_MiiSpeed100M : kENET_QOS_MiiSpeed10M;

                if (ENET_QOS_SetMII(EMAC, mii_speed, (enet_qos_mii_duplex_t)duplex) ==
                    kStatus_Success)
                {
                    s_last_speed = speed;
                    s_last_duplex = duplex;
                }
            }
            LOG_INF(DBG_M_ETH, "link up: %s %s",
                    (speed == kPHY_Speed1000M) ? "1000M" :
                    (speed == kPHY_Speed100M) ? "100M" : "10M",
                    (duplex == kPHY_FullDuplex) ? "full" : "half");
        }
    }
    else if (!link && s_link_up)
    {
        LOG_WRN(DBG_M_ETH, "link down");
    }
    s_link_up = link;
}

bool eth_raw_link_up(void)
{
    return s_link_up;
}

/* ---------------------------------------------------------------------- */
/* TX                                                                      */
/* ---------------------------------------------------------------------- */

status_t eth_raw_send(uint8_t *frame, uint32_t len)
{
    status_t status;
    uint32_t primask = __get_PRIMASK();

    /* Serialize descriptor/tail-pointer access between main loop, CAN ISRs
     * and the EMAC ISR. SendFrame is short (one descriptor + doorbell). */
    __disable_irq();
    /* Keep one descriptor slot permanently unused: with the wrap-safe
     * tail-pointer rule in ENET_QOS_SendFrame (see
     * docs/eqos-tx-ring-design.md) the tail must never
     * land on the oldest pending descriptor, or a completely full ring
     * would be indistinguishable from an empty one to the DMA. */
    if ((s_tx_submitted - s_tx_reclaimed) >= (E2CF_ETH_TXBD_NUM - 1U))
    {
        __set_PRIMASK(primask);
        s_stats.tx_busy++;
        return kStatus_ENET_QOS_TxFrameBusy;
    }
    status = ENET_QOS_SendFrame(EMAC, &s_handle, frame, len, 0U, false, NULL,
                                kENET_QOS_TxOffloadDisable);
    if (status == kStatus_Success)
    {
        s_tx_submitted++;
        s_stats.tx_frames++;
    }
    __set_PRIMASK(primask);

    if (status == kStatus_ENET_QOS_TxFrameBusy)
    {
        s_stats.tx_busy++;
    }
    return status;
}

uint32_t eth_raw_tx_submitted(void)
{
    return s_tx_submitted;
}

uint32_t eth_raw_tx_completed(void)
{
    return s_tx_reclaimed;
}

uint32_t eth_raw_tx_inflight(void)
{
    /* submitted (any context, masked) and reclaimed (EMAC ISR) move
     * independently; mask so the pair is coherent for ANY caller - the
     * stats dump calls this unmasked and could otherwise print an
     * underflowed count. */
    uint32_t primask = __get_PRIMASK();
    uint32_t n;

    __disable_irq();
    n = s_tx_submitted - s_tx_reclaimed;
    __set_PRIMASK(primask);
    return n;
}

const uint8_t *eth_raw_mac(void)
{
    return s_mac;
}

void eth_raw_dump_stats(void)
{
    LOG_INF(DBG_M_STATS, "ETH link=%u rx=%lu rxerr=%lu rxdrop=%lu tx=%lu txbusy=%lu infl=%lu",
            (unsigned)s_link_up, (unsigned long)s_stats.rx_frames,
            (unsigned long)s_stats.rx_errors, (unsigned long)s_stats.rx_drops,
            (unsigned long)s_stats.tx_frames, (unsigned long)s_stats.tx_busy,
            (unsigned long)eth_raw_tx_inflight());
}

void eth_raw_get_stats(eth_raw_stats_t *out)
{
    out->rx_frames = s_stats.rx_frames;
    out->rx_errors = s_stats.rx_errors;
    out->rx_drops  = s_stats.rx_drops;
    out->tx_frames = s_stats.tx_frames;
    out->tx_busy   = s_stats.tx_busy;
}

void eth_raw_clear_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
}
