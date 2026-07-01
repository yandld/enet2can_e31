/*
 * e2cf_config.h - build-time configuration of the E2CF gateway firmware
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Every tunable of the v1 firmware lives here so a deployment build is one
 * file diff. Defaults are bring-up friendly (channels auto-started, logging
 * on); a production build sets E2CF_AUTOSTART_CHANNELS=0 and lowers
 * E2CF_LOG_BUILD_LEVEL.
 */
#ifndef E2CF_CONFIG_H_
#define E2CF_CONFIG_H_

/* ---- firmware identity ---------------------------------------------- */
#define E2CF_FW_VERSION 0x01000000UL /* 1.0.0.0 */

/* ---- bring-up switches ----------------------------------------------- */
/*
 * Protocol §4.8: channels stay in STOP until the Linux side sends CFG START.
 * For standalone bench bring-up (no Linux peer yet) set 1: all channels are
 * configured 1M/5M BRS and STARTed at boot, and the safe-state TX gate is
 * not enforced until the first HB is ever received.
 */
#ifndef E2CF_AUTOSTART_CHANNELS
#define E2CF_AUTOSTART_CHANNELS 1
#endif

/* Internal loopback self-test mode for autostarted channels (no transceiver
 * or bus wiring needed; TX self-received - reuses the validated approach of
 * the enet2can_e31 project). Only meaningful with E2CF_AUTOSTART_CHANNELS=1. */
#ifndef E2CF_AUTOSTART_LOOPBACK
#define E2CF_AUTOSTART_LOOPBACK 0
#endif

/* Bitmask of channels brought up at boot / served at all. */
#ifndef E2CF_ACTIVE_CHAN_MASK
#define E2CF_ACTIVE_CHAN_MASK 0x3FU
#endif

/* Default bitrates for autostart (CFG SET_BITRATE overrides at runtime). */
#define E2CF_DEFAULT_NOM_BITRATE 1000000U
#define E2CF_DEFAULT_DAT_BITRATE 5000000U

/* ---- CAN resource plan (design doc 03 §3.3) --------------------------- */
/* All six instances use the same RX MB bank + single TX MB scheme. CAN0's
 * Enhanced RX FIFO (the only instance that has one) is parked: with the
 * eFIFO enabled, no RX interrupt ever fired on this silicon (the eFIFO
 * flags appear to route to an IRQ line this design does not serve), so
 * CAN0 uses the MB-bank path proven on CAN1-5. The eFIFO + eDMA batching
 * offload remains the Phase-2 item (design doc 03 deviation table). */

/* Per-instance RX MB bank depth (64-byte payload MBs), single TX MB. */
#define E2CF_RX_MB_CAN0 8U  /* CAN0: 96-MB instance, same depth as CAN1/2 */
#define E2CF_RX_MB_CAN12 8U /* CAN1/2: 64-MB instances */
#define E2CF_RX_MB_CAN345 4U /* CAN3/4/5: 32-MB instances */

/* ---- software queue depths ------------------------------------------- */
#define E2CF_SW_TXFIFO_DEPTH 16U /* = protocol window; Linux never exceeds it */
#define E2CF_RX_STAGE_DEPTH 64U  /* per-direction staging between ISR and agg */

/* ---- EQOS rings -------------------------------------------------------*/
/* RX ring depth raised 8 -> 16 to absorb the bursty downlink the host now
 * generates (canperf --performance batches with sendmmsg): an 8-deep ring
 * could momentarily overflow when the EMAC RX ISR is briefly held off by a
 * CAN ISR, dropping a downlink frame (seen as gw seq_lost). Each RX buffer is
 * a full 1536 B frame in cacheable SRAM and RXBUFF = RXBD*2, so 16 costs 48 KB
 * of s_rx_buff (vs 24.5 KB at 8); 64 would balloon to 192 KB and is overkill -
 * the wire serializes arrivals, so ~16 deep already covers any ISR-held window. */
#define E2CF_ETH_RXBD_NUM 16U
/* TX ring depth raised 8 -> 64: wire captures proved a rare TX
 * descriptor-ring WRAP anomaly (the frame queued in slot 0 vanishes and
 * an already-completed frame is retransmitted byte-identically; see
 * eth2can_design/eqos_tx_ring_wrap_report.md). A deeper ring cuts the
 * wrap rate 8x, and any residual loss moving to a mod-64 fingerprint
 * confirms the diagnosis. Costs +896 B of non-cacheable DTCM. */
#define E2CF_ETH_TXBD_NUM 64U
#define E2CF_ETH_RXBUFF_NUM (E2CF_ETH_RXBD_NUM * 2U)
/* TX frame staging faces (sealed frames waiting in/for the EQOS TX ring):
 * 2 may be open (DATA + TXC aggregation) + immediate HB/EVT/CFG/TIME sends,
 * leaving the rest as INFLIGHT headroom. Raised 6->8 to absorb the rare burst
 * (periodic send landing when the pool is transiently full) that showed as
 * starv +1 / lost 1 in a 1.5M-frame flood. The ncache region (16 KiB) holds
 * faces + EQOS BD rings: 8*1536 + 1024 + 128 = 13440 B < 16384, so this fits
 * without resizing RW_m_ncache; the free ceiling without a resize is 9. */
#define E2CF_ETH_TXFACE_NUM 8U

/* ---- NVIC priorities (design doc 03 §3.2; lower value = higher) ------- */
#define E2CF_IRQPRIO_CAN_RX 1U /* CAN0..5 MB (RX + TX-complete) */
/*
 * EMAC at the SAME level as CAN (no mutual preemption). The A/B disproved the
 * idea that dropping EMAC below CAN would lift FPS: at EMAC=2 the bus rate did
 * NOT improve (~6700 vs ~6900 fps) while loss exploded - face starvation
 * returned (starv +105) AND the now-preemptible EMAC RX ISR let the EQOS RX
 * ring overflow under CAN bursts (downlink seq_lost +332). Equal priority is
 * the better operating point: it balances CAN and EMAC so neither starves the
 * other. The residual tx_bus tail (TX-complete latency) is CAN-vs-CAN ISR
 * contention (TX-complete queued behind another channel's RX drain at the same
 * priority), independent of EMAC - so EMAC priority is not the FPS lever.
 */
#define E2CF_IRQPRIO_EMAC 1U
#define E2CF_IRQPRIO_SYSTICK 3U

/* ---- debug logging ----------------------------------------------------*/
/* Compile-time ceiling: statements above this level are removed entirely. */
#ifndef E2CF_LOG_BUILD_LEVEL
#define E2CF_LOG_BUILD_LEVEL E2CF_LOG_LEVEL_DEBUG
#endif
/* Runtime default level (changeable at runtime, see dbg_log.h). */
#ifndef E2CF_LOG_RUNTIME_LEVEL
#define E2CF_LOG_RUNTIME_LEVEL E2CF_LOG_LEVEL_INFO
#endif
/* Runtime default module mask (all on). */
#ifndef E2CF_LOG_RUNTIME_MASK
#define E2CF_LOG_RUNTIME_MASK 0xFFFFFFFFUL
#endif
/* Deferred-log ring size (bytes, power of two). ISRs format into this ring;
 * the main loop drains it to the UART so the data plane never blocks. */
#define E2CF_LOG_RING_SIZE 8192U
/* Max formatted length of one log line. */
#define E2CF_LOG_LINE_MAX 160U
/* Periodic UART statistics report interval, 0 = off (default).
 * Stats are pushed to Linux in-band at 1 Hz (E2CF_MSG_STATS, readable via
 * ethtool -S / debugfs) and remain dumpable on demand with the 's' console
 * key; set e.g. 3000 only for standalone bench bring-up without a peer. */
#ifndef E2CF_STATS_PERIOD_MS
#define E2CF_STATS_PERIOD_MS 0U
#endif

/* ---- data-plane profiler (gw_prof) ------------------------------------*/
/* 1 = compile in the in-firmware segment profiler (gw_prof.{h,c}): the data
 * path is instrumented at its key nodes and a per-segment timing/gauge table
 * is dumped on the 'p' console key (press 'p' to clear, run traffic, press 'p'
 * to read that window). Set 0 for a zero-overhead production build - every
 * gw_prof hook then compiles away. */
#ifndef E2CF_PROFILE
#define E2CF_PROFILE 1
#endif

#endif /* E2CF_CONFIG_H_ */
