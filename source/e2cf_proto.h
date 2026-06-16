/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * e2cf_proto.h - E2CF (Ethernet-to-CANFD tunnel) wire format, protocol v1
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * Naming: E2CF is the wire PROTOCOL name (this EtherType 0x88B5 framing and
 * the e2cf_* / E2CF_* symbols, normative spec = design doc 01); eth2can is
 * the Linux DRIVER and device name implementing it (eth2can.ko, eth2can0..5).
 *
 * SHARED between the MCXE31B firmware and the Linux eth2can driver - keep this
 * file free of SDK / kernel includes. Wire format is LITTLE-ENDIAN (both ends
 * are little-endian ARM; deliberately not network byte order, see the protocol
 * spec, eth2can_design doc 01 section 3). All multi-byte fields are naturally
 * aligned and every record is padded to a 4-byte boundary, so the packed
 * structs below can be overlaid on the wire buffer directly on both ends.
 *
 * Ethernet layer (spec §4.1): every E2CF frame carries an 802.1Q tag.
 *   DA(6) SA(6) TPID=0x8100(2) TCI(2) EtherType=0x88B5(2) E2CF-payload
 * PCP = 6 for DATA/TXC/EVT/TIME/HB, PCP = 2 for CFG_REQ/CFG_RSP/STATS.
 */
#ifndef E2CF_PROTO_H_
#define E2CF_PROTO_H_

#ifdef __KERNEL__
#include <linux/types.h> /* provides uintN_t */
#else
#include <stdint.h>
#endif

#define E2CF_ETHERTYPE       0x88B5U /* IEEE Std 802 local experimental 2 */
#define E2CF_TPID_8021Q      0x8100U
#define E2CF_VLAN_VID_DEFAULT 100U
#define E2CF_PCP_DATA        6U /* DATA/TXC/EVT/TIME/HB */
#define E2CF_PCP_CFG         2U /* CFG_REQ/CFG_RSP/STATS (management plane) */

#define E2CF_VERSION         1U
#define E2CF_NUM_CHANNELS    6U
#define E2CF_WIN_DEPTH       16U  /* per-channel sliding window = sw_txfifo depth */

/* Multicast DA usable for HB discovery only (spec §2.3). */
#define E2CF_HB_MCAST_DA     { 0x01U, 0xE2U, 0xCFU, 0x00U, 0x00U, 0x01U }

/* Message types (common header ver_type[3:0], spec §4.2). */
#define E2CF_MSG_DATA        0U
#define E2CF_MSG_TXC         1U
#define E2CF_MSG_EVT         2U
#define E2CF_MSG_CFG_REQ     3U
#define E2CF_MSG_CFG_RSP     4U
#define E2CF_MSG_TIME        5U
#define E2CF_MSG_HB          6U
#define E2CF_MSG_STATS       7U /* MCU->Linux stats push, 1 Hz (spec §4.9) */

/* Per-frame record count ceilings (spec §5). */
#define E2CF_DATA_MAX_RECORDS 17U
#define E2CF_TXC_MAX_RECORDS  32U
#define E2CF_EVT_MAX_RECORDS  6U

/* Aggregation timer, both directions; MCU->Linux mandatory (spec §6.1). */
#define E2CF_T_AGG_US        50U
/* Heartbeat period / link-supervision timeout (spec §4.8). */
#define E2CF_HB_PERIOD_MS    100U
#define E2CF_HB_TIMEOUT_MS   500U
/* CFG retry (spec §4.6). */
#define E2CF_CFG_TIMEOUT_MS  10U
#define E2CF_CFG_RETRIES     3U

#if defined(__GNUC__) || defined(__clang__) || defined(__CC_ARM)
#define E2CF_PACKED __attribute__((packed))
#else
#define E2CF_PACKED
#error "define E2CF_PACKED for this compiler"
#endif

/* ------------------------------------------------------------------ */
/* Common header (8 bytes, spec §4.2)                                  */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint8_t ver_type; /* [7:4]=version(1) [3:0]=message type */
    uint8_t count;    /* records in this frame */
    uint16_t seq;     /* per-direction sequence number, +1 per eth frame */
    uint32_t ts_base; /* sender clock ns, low 32 bits (frame time base) */
} e2cf_hdr_t;

#define E2CF_HDR_SIZE 8U

#define E2CF_VER_TYPE(ver, type) ((uint8_t)((((ver) & 0x0FU) << 4) | ((type) & 0x0FU)))
#define E2CF_HDR_VER(vt)  (((vt) >> 4) & 0x0FU)
#define E2CF_HDR_TYPE(vt) ((vt) & 0x0FU)

/* ------------------------------------------------------------------ */
/* DATA record (8-byte head + payload padded to 4B, spec §4.3)         */
/* Head is memory-layout-isomorphic with struct canfd_frame[0..7].     */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint32_t can_id; /* SocketCAN semantics: [28:0]=id [29]=0 [30]=RTR [31]=EFF */
    uint8_t len;     /* real payload byte count 0..64 (not DLC code) */
    uint8_t flags;   /* [0]=BRS [1]=ESI [2]=FDF. FDF=0 requires len<=8 and
                      * BRS=0 (violations rejected, TXC CTRL_ERROR); BRS=1
                      * also requires the channel to have a faster data phase.
                      * ESI is MCU->Linux only; senders set 0, MCU ignores. */
    uint8_t chan;    /* [2:0]=channel 0..5 */
    uint8_t tag;     /* Linux->MCU: echo_id 0..WIN-1; MCU->Linux: ts_off in 2us units, 0xFF=invalid */
    /* uint8_t data[ceil4(len)] follows; RTR: same sizing, data all-zero */
} e2cf_data_rec_t;

/* MCU->Linux DATA frames carry a 4-byte trailer AFTER the last record (not
 * counted in hdr->count): the gateway-clock low32 ns at which the frame was
 * handed to the EQOS TX (the "t4" eth-egress instant). With the request
 * frame's eth-rx instant carried back in the TXC (req_eth_rx_ns), the app
 * gets the full MCU residency incl. CAN bus as Y-X, both on the gateway
 * clock - an exact relative duration, no cross-clock mapping. */
#define E2CF_DATA_TX_TRAILER_SIZE 4U

#define E2CF_DATA_REC_HEAD_SIZE 8U
#define E2CF_DATA_FLAG_BRS 0x01U
#define E2CF_DATA_FLAG_ESI 0x02U
#define E2CF_DATA_FLAG_FDF 0x04U
#define E2CF_TAG_TS_INVALID 0xFFU
#define E2CF_TS_OFF_UNIT_NS 2000U /* tag ts_off unit: 2 us */

/* SocketCAN can_id bits (mirrors linux/can.h so the MCU side needs no kernel hdr) */
#define E2CF_CAN_EFF_FLAG 0x80000000UL
#define E2CF_CAN_RTR_FLAG 0x40000000UL
#define E2CF_CAN_ERR_FLAG 0x20000000UL
#define E2CF_CAN_SFF_MASK 0x000007FFUL
#define E2CF_CAN_EFF_MASK 0x1FFFFFFFUL

#define E2CF_CEIL4(n) ((((uint32_t)(n)) + 3U) & ~3UL)
#define E2CF_DATA_REC_SIZE(len) (E2CF_DATA_REC_HEAD_SIZE + E2CF_CEIL4(len))
#define E2CF_DATA_REC_SIZE_MAX  (E2CF_DATA_REC_HEAD_SIZE + 64U)

/* ------------------------------------------------------------------ */
/* TXC record (8 bytes, spec §4.4) - TX-complete confirm, MCU->Linux   */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint8_t chan;
    uint8_t tag;    /* echo_id of the confirmed DATA record */
    uint8_t status; /* e2cf_txc_status below */
    uint8_t rsv;
    uint32_t ts_off; /* TX-complete time - ts_base, ns; 0xFFFFFFFF=invalid */
    uint32_t req_eth_rx_ns; /* gateway-clock low32 ns at which the request
                             * frame that queued this send arrived at the MCU
                             * EMAC (the "t3" eth-ingress instant); pairs with
                             * the uplink DATA tx trailer to yield MCU
                             * residency Y-X on one clock. 0 = not available */
} e2cf_txc_rec_t;

#define E2CF_TXC_OK            0U
#define E2CF_TXC_ARB_LOST      1U /* one-shot mode: gave up after arbitration loss */
#define E2CF_TXC_CTRL_ERROR    2U
#define E2CF_TXC_CHAN_STOPPED  3U
#define E2CF_TXC_QUEUE_OVF     4U
#define E2CF_TXC_TS_INVALID    0xFFFFFFFFUL

/* ------------------------------------------------------------------ */
/* EVT record (12 bytes, spec §4.5) - channel state/errors, MCU->Linux */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint8_t chan;
    uint8_t state; /* e2cf_chan_state below */
    uint8_t tec;
    uint8_t rec;
    uint32_t err_flags; /* [0]=bit [1]=stuff [2]=crc [3]=form [4]=ack [5]=rx ovf [6]=tx ovf */
    uint16_t arb_lost_cnt;
    uint16_t rx_ovf_cnt;
} e2cf_evt_rec_t;

#define E2CF_STATE_ERROR_ACTIVE  0U
#define E2CF_STATE_ERROR_WARNING 1U
#define E2CF_STATE_ERROR_PASSIVE 2U
#define E2CF_STATE_BUS_OFF       3U

#define E2CF_ERR_BIT     (1UL << 0)
#define E2CF_ERR_STUFF   (1UL << 1)
#define E2CF_ERR_CRC     (1UL << 2)
#define E2CF_ERR_FORM    (1UL << 3)
#define E2CF_ERR_ACK     (1UL << 4)
#define E2CF_ERR_RX_OVF  (1UL << 5)
#define E2CF_ERR_TX_OVF  (1UL << 6)

/* ------------------------------------------------------------------ */
/* CFG_REQ / CFG_RSP body (24 bytes fixed, spec §4.6)                  */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint8_t token;  /* requestor-generated, echoed in RSP */
    uint8_t op;     /* e2cf_cfg_op below */
    uint8_t chan;   /* 0..5; 0xFF = global (GET_INFO / RESET only) */
    uint8_t status; /* REQ: 0; RSP: e2cf_cfg_status below */
    union E2CF_PACKED
    {
        uint8_t raw[20];
        struct E2CF_PACKED /* op=SET_BITRATE (REQ; RSP echoes applied values) */
        {
            uint16_t nom_brp;   /* arbitration prescaler, PE clock 80 MHz */
            uint8_t nom_tseg1;  /* PROP_SEG + PHASE_SEG1, in tq */
            uint8_t nom_tseg2;
            uint8_t nom_sjw;
            uint8_t dat_brp;
            uint8_t dat_tseg1;
            uint8_t dat_tseg2;
            uint8_t dat_sjw;
            uint8_t tdc_off;    /* TDC offset (tq); 0=firmware default (BRS on a
                                 * real bus: measured TDC on; loopback: TDC off) */
            uint16_t mode_flags; /* see E2CF_MODE_*; when the FD bit is clear
                                  * the dat_ fields and tdc_off are ignored
                                  * (senders should fill them with 0) */
            uint8_t rsv[8];
        } bitrate;
        struct E2CF_PACKED /* op=SET_FILTER */
        {
            uint8_t bank; /* 0..7 */
            uint8_t rsv0;
            uint16_t rsv1;
            uint32_t can_id;
            uint32_t can_mask; /* pass if (recv_id & mask) == (can_id & mask) */
            uint8_t rsv2[8];
        } filter;
        struct E2CF_PACKED /* op=GET_INFO (RSP) */
        {
            uint32_t fw_ver;
            uint8_t proto_ver;
            uint8_t nchan;
            uint8_t win_depth;
            uint8_t rsv0;
            uint32_t can_clk_hz; /* 80000000 */
            uint32_t cap_flags;
            uint8_t rsv1[4];
        } info;
    } u;
} e2cf_cfg_body_t;

#define E2CF_CFG_BODY_SIZE 24U

#define E2CF_CFG_OP_SET_BITRATE 1U
#define E2CF_CFG_OP_SET_MODE    2U
#define E2CF_CFG_OP_SET_FILTER  3U
#define E2CF_CFG_OP_START       4U
#define E2CF_CFG_OP_STOP        5U
#define E2CF_CFG_OP_RESET       6U
#define E2CF_CFG_OP_GET_STATUS  7U
#define E2CF_CFG_OP_GET_INFO    8U
#define E2CF_CFG_OP_CLEAR_STATS 9U /* chan 0..5 or 0xFF=global; zeroes counters
                                    * only - channel state / controller untouched
                                    * (unlike RESET). spec §4.6 (v1.0-draft3) */

#define E2CF_CFG_OK             0U
#define E2CF_CFG_EINVAL         1U
#define E2CF_CFG_EBUSY          2U /* channel running, STOP first */
#define E2CF_CFG_ENOTSUP        3U
#define E2CF_CFG_EINTERNAL      4U

#define E2CF_MODE_FD            (1U << 0)
#define E2CF_MODE_NONISO        (1U << 1) /* v1 firmware: ENOTSUP (ISO FD only) */
#define E2CF_MODE_LISTENONLY    (1U << 2)
#define E2CF_MODE_LOOPBACK      (1U << 3)
#define E2CF_MODE_ONESHOT       (1U << 4) /* v1 firmware: ENOTSUP */

#define E2CF_CFG_CHAN_GLOBAL    0xFFU
#define E2CF_CAN_CLK_HZ         80000000UL

/* GET_INFO cap_flags bits */
#define E2CF_CAP_STATS          (1UL << 0) /* STATS push + CLEAR_STATS supported */

/* ------------------------------------------------------------------ */
/* TIME body (16 bytes, spec §4.7) - full gateway clock, 1 Hz          */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint64_t full_time_ns; /* gateway monotonic clock, full ns */
    uint32_t flags;        /* [0]=synced to 1588 */
    uint32_t rsv;
} e2cf_time_body_t;

#define E2CF_TIME_BODY_SIZE 16U

/* ------------------------------------------------------------------ */
/* HB body (8 bytes, spec §4.8) - heartbeat, both directions, 100 ms   */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint8_t state; /* 0=init 1=ready 2=fault */
    uint8_t nchan;
    uint16_t rsv;
    uint32_t uptime_s;
} e2cf_hb_body_t;

#define E2CF_HB_BODY_SIZE 8U
#define E2CF_HB_STATE_INIT  0U
#define E2CF_HB_STATE_READY 1U
#define E2CF_HB_STATE_FAULT 2U

/* ------------------------------------------------------------------ */
/* STATS body (spec §4.9, v1.0-draft3) - gateway statistics push,      */
/* MCU->Linux, 1 Hz, PCP 2, count=1, one body per frame.               */
/*                                                                     */
/* Layout: e2cf_stats_hdr_t, then one global block, then nchan channel */
/* blocks. Forward compatibility: receivers locate channel block i at  */
/* hdr_size + global_len + i*chan_len using the lengths FROM THE WIRE, */
/* and parse min(wire length, local sizeof) of each block. New fields  */
/* append at block ends only; layout_rev bumps only if existing fields */
/* change meaning.                                                     */
/* ------------------------------------------------------------------ */
typedef struct E2CF_PACKED
{
    uint8_t layout_rev;  /* E2CF_STATS_LAYOUT_REV */
    uint8_t nchan;       /* channel blocks following the global block */
    uint16_t global_len; /* size of the global block as built by sender */
    uint16_t chan_len;   /* stride of one channel block */
    uint16_t rsv0;
    uint64_t gw_time_ns; /* gateway monotonic clock at fill time (same
                          * clock as TIME); uptime_s = gw_time_ns / 1e9 */
} e2cf_stats_hdr_t;

typedef struct E2CF_PACKED /* gateway-global counters since boot/clear */
{
    uint32_t eth_rx_frames;   /* accepted E2CF eth frames */
    uint32_t eth_rx_bad;      /* bad version / malformed */
    uint32_t eth_rx_untagged; /* untagged frames tolerated */
    uint32_t seq_gaps;        /* Linux->MCU sequence gaps observed */
    uint32_t data_rx_recs;    /* Linux->MCU DATA records accepted */
    uint32_t data_rx_rejects; /* Linux->MCU DATA records rejected (TXC'd) */
    uint32_t data_tx_recs;    /* MCU->Linux DATA records uploaded */
    uint32_t txc_recs;        /* TXC records emitted */
    uint32_t rx_gated_drops;  /* CAN RX dropped while in safe state */
    uint32_t face_starved;    /* TX face pool exhaustion events */
    uint32_t frames_sent;     /* E2CF eth frames sent */
    uint32_t send_fail;       /* eth TX ring full at frame send */
    uint32_t cfg_reqs;        /* CFG_REQ frames processed */
    uint32_t hb_rx;
    uint32_t hb_tx;
    uint32_t emac_rx_frames;  /* EMAC driver level */
    uint32_t emac_rx_errors;
    uint32_t emac_rx_drops;
    uint32_t emac_tx_frames;
    uint32_t emac_tx_busy;
    uint32_t dbg_log_dropped; /* UART log ring overflow lines */
    uint8_t flags;            /* see E2CF_STATS_F_* */
    uint8_t rsv[3];
    uint32_t seq_lost;        /* Linux->MCU frames confirmed lost: holes
                               * pushed out of the 64-frame reorder window
                               * still unfilled (seq_gaps counts raw gap
                               * EVENTS and fires on mere reordering) */
    uint32_t seq_reorder;     /* Linux->MCU frames that arrived out of
                               * order (filled a window hole) */
    uint32_t loop_per_s;      /* gateway superloop iterations during the
                               * last STATS interval: an uncalibrated CPU
                               * headroom gauge - compare against the
                               * idle-system baseline of the same build */
} e2cf_stats_global_t;

typedef struct E2CF_PACKED /* per-channel counters since boot/clear */
{
    uint32_t rx_frames;   /* CAN frames received from the bus */
    uint32_t rx_ovf;      /* RX mailbox overrun losses */
    uint32_t tx_frames;   /* CAN frames completed on the bus */
    uint32_t tx_rejected; /* submit-time rejects (validation/window/stopped) */
    uint32_t tx_timeout;  /* stuck-TX watchdog aborts (CTRL_ERROR TXCs) */
    uint32_t fifo_hwm;    /* sw_txfifo high-water mark */
    uint32_t irq_count;
    uint32_t bus_off_cnt;
    uint8_t state;        /* E2CF_STATE_* */
    uint8_t tec;          /* live bus error counters */
    uint8_t rec;
    uint8_t flags;        /* [0]=channel running */
    uint16_t arb_lost_cnt; /* reserved live: 0 in v1 (no one-shot mode) */
    uint16_t rx_ovf_cnt;   /* same counter EVT carries */
} e2cf_stats_chan_t;

#define E2CF_STATS_LAYOUT_REV  1U /* unchanged: fields only appended */
#define E2CF_STATS_HDR_SIZE    16U
#define E2CF_STATS_GLOBAL_SIZE 100U
#define E2CF_STATS_CHAN_SIZE   40U

#define E2CF_STATS_F_PEER_LOCKED (1U << 0)
#define E2CF_STATS_F_SAFE_STATE  (1U << 1)
#define E2CF_STATS_F_ETH_LINK    (1U << 2)

#define E2CF_STATS_CHAN_F_RUNNING (1U << 0)

#define E2CF_STATS_BODY_SIZE \
    (E2CF_STATS_HDR_SIZE + E2CF_STATS_GLOBAL_SIZE + \
     (E2CF_NUM_CHANNELS * E2CF_STATS_CHAN_SIZE))

/* ------------------------------------------------------------------ */
/* Frame size helpers                                                  */
/* ------------------------------------------------------------------ */
/* DA+SA+TPID+TCI+EtherType */
#define E2CF_L2_HDR_SIZE   18U
#define E2CF_MTU           1500U
/* Largest E2CF payload: hdr + 17 * 72B DATA records = 1232B, < MTU. */
#define E2CF_MAX_PAYLOAD   (E2CF_HDR_SIZE + (E2CF_DATA_MAX_RECORDS * E2CF_DATA_REC_SIZE_MAX))
#define E2CF_MAX_FRAME     (E2CF_L2_HDR_SIZE + E2CF_MAX_PAYLOAD)

#endif /* E2CF_PROTO_H_ */
