/*
 * e2cf_core.c - E2CF protocol engine (aggregation, window, CFG, HB)
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Concurrency model: appenders run at CAN IRQ priority (1), the eth demux at
 * EMAC IRQ priority (2), periodic work in the main loop. Every access to the
 * aggregation/face state takes a short global-IRQ-masked critical section
 * (tens of cycles) - the same discipline the design doc budgets for.
 *
 * ts_base convention (documented implementation choice): ts_base is stamped
 * when the FIRST record enters the frame, so every record's ts_off >= 0 and
 * fits the 2 us * 255 (DATA) / ns u32 (TXC) encodings within the 50 us
 * aggregation window.
 */
#include "e2cf_core.h"

#include <string.h>

#include "can_hw.h"
#include "dbg_log.h"
#include "e2cf_config.h"
#include "e2cf_proto.h"
#include "eth_raw.h"
#include "fsl_common.h"
#include "gw_prof.h"
#include "gw_time.h"

/* Wire-layout guards for the STATS body (spec §4.9). */
_Static_assert(sizeof(e2cf_stats_hdr_t) == E2CF_STATS_HDR_SIZE,
               "e2cf_stats_hdr_t layout drift");
_Static_assert(sizeof(e2cf_stats_global_t) == E2CF_STATS_GLOBAL_SIZE,
               "e2cf_stats_global_t layout drift");
_Static_assert(sizeof(e2cf_stats_chan_t) == E2CF_STATS_CHAN_SIZE,
               "e2cf_stats_chan_t layout drift");

/* ---------------------------------------------------------------------- */
/* TX faces (frame staging buffers, DMA-read zero-copy)                    */
/* ---------------------------------------------------------------------- */

#define FACE_SIZE 1536U

typedef enum
{
    FACE_FREE = 0,
    FACE_FILLING,
    FACE_INFLIGHT
} face_state_t;

/* DTCM non-cacheable: EQOS DMA reads faces in place, no cache maintenance. */
AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t s_face[E2CF_ETH_TXFACE_NUM][FACE_SIZE], 64U);
static volatile uint8_t s_face_state[E2CF_ETH_TXFACE_NUM];
/* EQOS submit serial of each INFLIGHT face's frame; face frees once the TX
 * completed count reaches it (per-frame reclaim, see face_reclaim). */
static uint32_t s_face_serial[E2CF_ETH_TXFACE_NUM];

/* Aggregation context for the two aggregated message types. */
typedef struct
{
    int8_t face;          /* -1 = closed */
    uint8_t msg_type;
    uint8_t count;
    uint16_t cursor;      /* write offset within the face */
    uint32_t ts_base;      /* ns lo32 at first record */
    uint32_t deadline;    /* gw_cycles() of the T_agg expiry */
    uint32_t open_cyc;    /* gw_cycles() when the face was opened (gw_prof) */
} agg_t;

/* Closed sentinel (face=-1) true from reset, independent of init order: a
 * future bring-up reordering cannot leave the BSS-zero value 0 masquerading
 * as an open face. agg_reset() re-establishes the same state at runtime. */
static agg_t s_agg_data = { .face = -1 }; /* MCU->Linux DATA */
static agg_t s_agg_txc  = { .face = -1 }; /* MCU->Linux TXC */

static uint16_t s_tx_seq;
static uint8_t s_peer_mac[6];
static volatile bool s_peer_locked;
static volatile uint32_t s_last_hb_rx_cycles;
static volatile bool s_ever_saw_hb;
static volatile bool s_safe_state; /* HB timeout: gate CAN TX, drop CAN RX */

static uint16_t s_rx_seq_expected;
static bool s_rx_seq_valid;
/* Frame-sequence loss tracker (single EMAC ISR context, no locking):
 * 64-frame sliding window telling losses apart from reorders - see
 * seq_track() below. */
static uint16_t s_rx_seq_hi;  /* highest sequence number seen */
static uint64_t s_rx_seq_win; /* bit i = frame (s_rx_seq_hi - i) arrived */

/* Superloop pass counter for the STATS loop_per_s headroom gauge. */
static uint32_t s_loop_iters;
static uint32_t s_loop_last;

static struct
{
    uint32_t eth_rx_frames;
    uint32_t eth_rx_bad;     /* wrong version / malformed */
    uint32_t eth_rx_untagged;
    uint32_t seq_gaps;
    uint32_t seq_lost;       /* confirmed lost (hole expired unfilled) */
    uint32_t seq_reorder;    /* arrived out of order (filled a hole) */
    uint32_t data_rx_recs;   /* Linux->MCU send requests accepted */
    uint32_t data_rx_rejects;
    uint32_t data_tx_recs;   /* MCU->Linux CAN frames uploaded */
    uint32_t txc_recs;
    uint32_t rx_gated_drops; /* safe-state drops */
    uint32_t face_starved;
    uint32_t frames_sent;
    uint32_t send_fail;
    uint32_t cfg_reqs;
    uint32_t hb_rx;
    uint32_t hb_tx;
} s_st;

static const uint8_t s_hb_mcast[6] = E2CF_HB_MCAST_DA;

/* CFG idempotency: last token + cached RSP per channel slot (6 + global). */
typedef struct
{
    bool valid;
    uint8_t token;
    e2cf_cfg_body_t rsp;
} cfg_cache_t;
static cfg_cache_t s_cfg_cache[E2CF_NUM_CHANNELS + 1U];

/* ---------------------------------------------------------------------- */
/* Frame building                                                          */
/* ---------------------------------------------------------------------- */

/*
 * Grab a FREE TX face and mark it FILLING; returns -1 when the pool is
 * exhausted (counted as face_starved).
 */
static int8_t face_alloc(void)
{
    for (uint8_t i = 0U; i < E2CF_ETH_TXFACE_NUM; i++)
    {
        if (s_face_state[i] == (uint8_t)FACE_FREE)
        {
            s_face_state[i] = (uint8_t)FACE_FILLING;
            return (int8_t)i;
        }
    }
    s_st.face_starved++;
    return -1;
}

/*
 * Per-frame face reclaim: free each INFLIGHT face as soon as ITS OWN frame's
 * TX DMA completes. Frames complete in submission order (single FIFO ring),
 * so a face whose stamped submit serial has been reached by the completed
 * count is done with its DMA read and may be reused. Replaces the old
 * all-or-nothing barrier (free everything only when the whole ring drained
 * to inflight==0), which starved the 6-face pool under concurrent load
 * because the ring rarely emptied. Signed 32-bit compare is wrap-safe.
 */
static void face_reclaim(void)
{
    uint32_t completed = eth_raw_tx_completed();

    for (uint8_t i = 0U; i < E2CF_ETH_TXFACE_NUM; i++)
    {
        if ((s_face_state[i] == (uint8_t)FACE_INFLIGHT) &&
            ((int32_t)(completed - s_face_serial[i]) >= 0))
        {
            s_face_state[i] = (uint8_t)FACE_FREE;
        }
    }
}

/* Write L2 + E2CF headers; returns payload offset of the first record. */
static uint16_t frame_init(uint8_t *buf, uint8_t msg_type, const uint8_t *da)
{
    uint8_t pcp = ((msg_type == E2CF_MSG_CFG_REQ) || (msg_type == E2CF_MSG_CFG_RSP) ||
                   (msg_type == E2CF_MSG_STATS))
                      ? E2CF_PCP_CFG : E2CF_PCP_DATA;
    uint16_t tci = (uint16_t)(((uint16_t)pcp << 13) | (E2CF_VLAN_VID_DEFAULT & 0x0FFFU));
    e2cf_hdr_t *hdr;

    memcpy(&buf[0], da, 6);
    memcpy(&buf[6], eth_raw_mac(), 6);
    buf[12] = (uint8_t)(E2CF_TPID_8021Q >> 8);
    buf[13] = (uint8_t)(E2CF_TPID_8021Q & 0xFFU);
    buf[14] = (uint8_t)(tci >> 8);
    buf[15] = (uint8_t)(tci & 0xFFU);
    buf[16] = (uint8_t)(E2CF_ETHERTYPE >> 8);
    buf[17] = (uint8_t)(E2CF_ETHERTYPE & 0xFFU);

    hdr = (e2cf_hdr_t *)&buf[E2CF_L2_HDR_SIZE];
    hdr->ver_type = E2CF_VER_TYPE(E2CF_VERSION, msg_type);
    hdr->count = 0U;
    hdr->seq = 0U;     /* sealed at send */
    hdr->ts_base = 0U; /* sealed at send / first record */

    return E2CF_L2_HDR_SIZE + E2CF_HDR_SIZE;
}

static const uint8_t *peer_da(void)
{
    return s_peer_locked ? s_peer_mac : s_hb_mcast;
}

/* Finalize and transmit a filled face. IRQ-masked callers. */
static void frame_send(int8_t face, uint16_t total_len, uint8_t count, uint32_t ts_base)
{
    uint8_t *buf = s_face[face];
    e2cf_hdr_t *hdr = (e2cf_hdr_t *)&buf[E2CF_L2_HDR_SIZE];

#if E2CF_PROFILE
    /* EQOS TX ring occupancy at the moment we hand this frame to the DMA: a
     * gauge of egress backpressure. */
    gw_prof_add(PROF_ETH_INFLIGHT, eth_raw_tx_inflight());
#endif

    hdr->count = count;
    hdr->seq = s_tx_seq++;
    hdr->ts_base = ts_base;

    /* DATA frames carry a 4-byte eth-egress trailer (the "t4" instant): the
     * gw-clock low32 ns captured here, just before handing the frame to the
     * EQOS TX. Not counted in hdr->count; the host reads it at payload tail.
     * Records cap at 17 (E2CF_DATA_MAX_RECORDS), far below FACE_SIZE, so the
     * extra 4 bytes never risk overflow. */
    if (E2CF_HDR_TYPE(hdr->ver_type) == E2CF_MSG_DATA)
    {
        uint32_t tx_lo = gw_now_ns_lo32();

        memcpy(&buf[total_len], &tx_lo, sizeof(tx_lo));
        total_len += (uint16_t)E2CF_DATA_TX_TRAILER_SIZE;
    }

    if (total_len < 60U)
    {
        memset(&buf[total_len], 0, 60U - total_len); /* min frame padding */
        total_len = 60U;
    }

    s_face_state[face] = (uint8_t)FACE_INFLIGHT;
    if (eth_raw_send(buf, total_len) == kStatus_Success)
    {
        /* Stamp this frame's submit serial (read under the same IRQ mask the
         * caller holds, so no concurrent submit can race it). The face frees
         * in face_reclaim once eth_raw_tx_completed() reaches this serial. */
        s_face_serial[face] = eth_raw_tx_submitted();
        s_st.frames_sent++;
    }
    else
    {
        s_st.send_fail++;
        s_face_state[face] = (uint8_t)FACE_FREE; /* ring full: frame dropped */
    }
}

/* ---------------------------------------------------------------------- */
/* Aggregation (DATA / TXC)                                                */
/* ---------------------------------------------------------------------- */

/*
 * Reset an aggregation context to the closed state for the given message type.
 */
static void agg_reset(agg_t *agg, uint8_t msg_type)
{
    agg->face = -1;
    agg->msg_type = msg_type;
    agg->count = 0U;
    agg->cursor = 0U;
    agg->ts_base = 0U;
    agg->deadline = 0U;
}

/* Seal + send the open aggregation frame. IRQ-masked callers. */
static void agg_flush(agg_t *agg)
{
    if (agg->face < 0)
    {
        return;
    }
#if E2CF_PROFILE
    /* Uplink aggregation diagnostics (DATA only): how long the oldest record
     * waited in the face (open -> flush, bounded by T_agg) and how many records
     * this frame batched. */
    if (agg->msg_type == E2CF_MSG_DATA)
    {
        gw_prof_add(PROF_FACE_DWELL, gw_cycles() - agg->open_cyc);
        gw_prof_add(PROF_AGG_RECORDS, agg->count);
    }
#endif
    frame_send(agg->face, agg->cursor, agg->count, agg->ts_base);
    agg->face = -1;
    agg->count = 0U;
    agg->cursor = 0U;
}

/*
 * Reserve space for one record in the aggregation frame; opens a new frame
 * when needed. Returns NULL when no face is available (overload). The caller
 * fills the returned pointer and then calls agg_commit(). IRQ-masked callers.
 */
static uint8_t *agg_reserve(agg_t *agg, uint16_t rec_size, uint8_t max_records, uint32_t now_ns)
{
    if ((agg->face >= 0) &&
        ((agg->count >= max_records) || ((agg->cursor + rec_size) > FACE_SIZE)))
    {
        agg_flush(agg);
    }

    if (agg->face < 0)
    {
        int8_t face;

        face_reclaim();
        face = face_alloc();
        if (face < 0)
        {
            return NULL;
        }
        agg->face = face;
        agg->cursor = frame_init(s_face[face], agg->msg_type, peer_da());
        agg->count = 0U;
        agg->ts_base = now_ns;
        agg->deadline = gw_cycles() + (E2CF_T_AGG_US * gw_cycles_per_us());
#if E2CF_PROFILE
        agg->open_cyc = gw_cycles();
#endif
    }

    return &s_face[agg->face][agg->cursor];
}

/*
 * Account one freshly written record; flush only when the frame is full.
 *
 * Time-bounded batching: a partially filled frame is sealed by the T_agg
 * (50 us) deadline check in e2cf_core_poll(), or by the next record that
 * would overflow the face (agg_reserve). The previous "flush immediately
 * when eth_raw_tx_inflight()==0" rule defeated batching entirely - at
 * 100M the TX ring drains a frame in ~7 us, so the egress is "idle"
 * between almost every pair of CAN frames, forcing 1 record per ethernet
 * frame. That churned the 6-face pool at the full CAN frame rate (17x the
 * designed rate) and starved it under concurrent load. Relying on T_agg
 * instead restores aggregation and keeps face consumption bounded; the
 * cost is up to T_agg of extra uplink latency, which is the spec budget.
 */
static void agg_commit(agg_t *agg, uint16_t rec_size, uint8_t max_records)
{
    agg->cursor += rec_size;
    agg->count++;

    if (agg->count >= max_records)
    {
        agg_flush(agg);
    }
}

/* ---------------------------------------------------------------------- */
/* Upcalls from can_hw (CAN IRQ, prio 1)                                   */
/* ---------------------------------------------------------------------- */

/*
 * CAN-IRQ upcall: append one received CAN frame as a DATA record to the uplink
 * aggregation (drops while in safe state).
 */
void e2cf_core_can_rx(uint8_t ch, uint32_t can_id, uint8_t len, uint8_t flags,
                      const uint8_t *data, uint32_t rx_ns_lo32)
{
    uint16_t rec_size = (uint16_t)E2CF_DATA_REC_SIZE(len);
    uint8_t *p;
    e2cf_data_rec_t *rec;
    uint32_t primask;

    if (s_safe_state)
    {
        s_st.rx_gated_drops++;
        LOG_DBG(DBG_M_PROTO, "rx gated (safe state): chan=%u id=0x%08lx", ch,
                (unsigned long)can_id);
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    p = agg_reserve(&s_agg_data, rec_size, E2CF_DATA_MAX_RECORDS, rx_ns_lo32);
    if (p == NULL)
    {
        __set_PRIMASK(primask);
        LOG_DBG(DBG_M_PROTO, "up drop (face starved): chan=%u", ch);
        return;
    }

    rec = (e2cf_data_rec_t *)p;
    rec->can_id = can_id;
    rec->len = len;
    rec->flags = flags;
    rec->chan = ch;
    {
        uint32_t off = (rx_ns_lo32 - s_agg_data.ts_base) / E2CF_TS_OFF_UNIT_NS;

        rec->tag = (off > 0xFEU) ? E2CF_TAG_TS_INVALID : (uint8_t)off;
    }
    if (len != 0U)
    {
        memcpy(&p[E2CF_DATA_REC_HEAD_SIZE], data, len);
        if ((len & 3U) != 0U)
        {
            memset(&p[E2CF_DATA_REC_HEAD_SIZE + len], 0, 4U - (len & 3U));
        }
    }
    agg_commit(&s_agg_data, rec_size, E2CF_DATA_MAX_RECORDS);
    s_st.data_tx_recs++;
    __set_PRIMASK(primask);
    LOG_DBG(DBG_M_PROTO, "up rec: chan=%u id=0x%08lx len=%u", ch,
            (unsigned long)can_id, len);
}

/*
 * CAN-IRQ upcall: append one TX-complete confirmation to the TXC aggregation.
 */
void e2cf_core_can_txc(uint8_t ch, uint8_t tag, uint8_t status, uint32_t ns_lo32,
                       uint32_t req_eth_rx_ns)
{
    uint8_t *p;
    e2cf_txc_rec_t *rec;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    p = agg_reserve(&s_agg_txc, (uint16_t)sizeof(e2cf_txc_rec_t), E2CF_TXC_MAX_RECORDS, ns_lo32);
    if (p == NULL)
    {
        __set_PRIMASK(primask);
        return; /* face starvation; Linux window recovers via its own timeout */
    }
    rec = (e2cf_txc_rec_t *)p;
    rec->chan = ch;
    rec->tag = tag;
    rec->status = status;
    rec->rsv = 0U;
    rec->req_eth_rx_ns = req_eth_rx_ns; /* "t3" eth-ingress instant of the request */
    {
        /* ns_lo32 was sampled by the caller BEFORE this masked section; a
         * preempting CAN ISR may have opened a fresh TXC frame with a newer
         * ts_base. Clamp the underflow to 0 (same-frame, sub-resolution skew)
         * instead of shipping a ~4e9 ns offset on the wire. */
        uint32_t off = ns_lo32 - s_agg_txc.ts_base;

        rec->ts_off = ((int32_t)off < 0) ? 0U : off;
    }
    agg_commit(&s_agg_txc, (uint16_t)sizeof(e2cf_txc_rec_t), E2CF_TXC_MAX_RECORDS);
    s_st.txc_recs++;
    __set_PRIMASK(primask);
}

/* Build + send one immediate (non-aggregated) message. Any context. */
static void send_immediate(uint8_t msg_type, const void *body, uint16_t body_len,
                           uint8_t count, const uint8_t *da)
{
    uint32_t primask = __get_PRIMASK();
    int8_t face;
    uint16_t cursor;

    __disable_irq();
    face_reclaim();
    face = face_alloc();
    if (face >= 0)
    {
        cursor = frame_init(s_face[face], msg_type, da);
        memcpy(&s_face[face][cursor], body, body_len);
        frame_send(face, (uint16_t)(cursor + body_len), count, gw_now_ns_lo32());
    }
    __set_PRIMASK(primask);
}

void e2cf_core_can_state_change(uint8_t ch, const e2cf_evt_rec_t *evt)
{
    (void)ch;
    send_immediate(E2CF_MSG_EVT, evt, (uint16_t)sizeof(*evt), 1U, peer_da());
}

/* ---------------------------------------------------------------------- */
/* Ethernet RX demux (EMAC IRQ, prio 2)                                    */
/* ---------------------------------------------------------------------- */

/*
 * Walk the Linux->MCU DATA records, validate sizes, and submit each frame to
 * can_hw; rejected submissions are confirmed immediately with a terminal TXC.
 */
static void handle_data_msg(const uint8_t *rec, uint16_t remain, uint8_t count,
                            uint32_t eth_rx_lo32)
{
    while ((count != 0U) && (remain >= E2CF_DATA_REC_HEAD_SIZE))
    {
        const e2cf_data_rec_t *head = (const e2cf_data_rec_t *)rec;
        uint16_t rec_size;
        uint8_t status;

        if (head->len > 64U)
        {
            s_st.eth_rx_bad++;
            return;
        }
        rec_size = (uint16_t)E2CF_DATA_REC_SIZE(head->len);
        if (rec_size > remain)
        {
            s_st.eth_rx_bad++;
            return;
        }

        status = can_hw_submit_tx(head, &rec[E2CF_DATA_REC_HEAD_SIZE], eth_rx_lo32);
        if (status == E2CF_TXC_OK)
        {
            s_st.data_rx_recs++;
        }
        else
        {
            /* Terminal reject: confirm the window slot right away. */
            s_st.data_rx_rejects++;
            e2cf_core_can_txc(head->chan & 0x07U, head->tag, status,
                              gw_now_ns_lo32(), eth_rx_lo32);
        }

        rec += rec_size;
        remain -= rec_size;
        count--;
    }
}

/*
 * Execute one CFG_REQ operation against can_hw and fill in the response body.
 * Runs synchronously in the EMAC ISR.
 */
static void cfg_execute(const e2cf_cfg_body_t *req, e2cf_cfg_body_t *rsp)
{
    uint8_t ch = req->chan;

    memset(rsp, 0, sizeof(*rsp));
    rsp->token = req->token;
    rsp->op = req->op;
    rsp->chan = req->chan;
    rsp->status = E2CF_CFG_OK;

    switch (req->op)
    {
        case E2CF_CFG_OP_SET_BITRATE:
        {
            can_hw_bittiming_t bt;

            bt.nom_brp = req->u.bitrate.nom_brp;
            bt.nom_tseg1 = req->u.bitrate.nom_tseg1;
            bt.nom_tseg2 = req->u.bitrate.nom_tseg2;
            bt.nom_sjw = req->u.bitrate.nom_sjw;
            bt.dat_brp = req->u.bitrate.dat_brp;
            bt.dat_tseg1 = req->u.bitrate.dat_tseg1;
            bt.dat_tseg2 = req->u.bitrate.dat_tseg2;
            bt.dat_sjw = req->u.bitrate.dat_sjw;
            bt.tdc_off = req->u.bitrate.tdc_off;
            bt.mode_flags = req->u.bitrate.mode_flags;
            rsp->status = can_hw_set_bitrate(ch, &bt);
            if (rsp->status == E2CF_CFG_OK)
            {
                rsp->u.bitrate = req->u.bitrate; /* echo applied values */
            }
            break;
        }
        case E2CF_CFG_OP_SET_MODE:
        {
            /* mode_flags-only update; same field as SET_BITRATE offset 14. */
            can_hw_bittiming_t bt;

            can_hw_get_bitrate(ch, &bt);
            bt.mode_flags = req->u.bitrate.mode_flags;
            rsp->status = can_hw_set_bitrate(ch, &bt);
            break;
        }
        case E2CF_CFG_OP_SET_FILTER:
            rsp->status = can_hw_set_filter(ch, req->u.filter.bank,
                                            req->u.filter.can_id, req->u.filter.can_mask);
            break;
        case E2CF_CFG_OP_START:
            rsp->status = can_hw_start(ch);
            break;
        case E2CF_CFG_OP_STOP:
            rsp->status = can_hw_stop(ch);
            break;
        case E2CF_CFG_OP_RESET:
            if (ch == E2CF_CFG_CHAN_GLOBAL)
            {
                for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
                {
                    (void)can_hw_reset(i);
                }
            }
            else
            {
                rsp->status = can_hw_reset(ch);
            }
            break;
        case E2CF_CFG_OP_GET_STATUS:
        {
            e2cf_evt_rec_t evt;

            if (ch >= E2CF_NUM_CHANNELS)
            {
                rsp->status = E2CF_CFG_EINVAL;
                break;
            }
            can_hw_get_evt(ch, &evt);
            rsp->u.raw[0] = evt.state;
            rsp->u.raw[1] = evt.tec;
            rsp->u.raw[2] = evt.rec;
            rsp->u.raw[3] = can_hw_is_running(ch) ? 1U : 0U;
            break;
        }
        case E2CF_CFG_OP_GET_INFO:
            rsp->u.info.fw_ver = E2CF_FW_VERSION;
            rsp->u.info.proto_ver = E2CF_VERSION;
            rsp->u.info.nchan = E2CF_NUM_CHANNELS;
            rsp->u.info.win_depth = E2CF_WIN_DEPTH;
            rsp->u.info.can_clk_hz = E2CF_CAN_CLK_HZ;
            rsp->u.info.cap_flags = E2CF_CAP_STATS;
            break;
        case E2CF_CFG_OP_CLEAR_STATS:
            /* Counters only; channel/controller state untouched. The global
             * clear includes the request that triggered it, so cfg_reqs
             * reads 0 right after - intended. */
            if (ch == E2CF_CFG_CHAN_GLOBAL)
            {
                for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
                {
                    (void)can_hw_clear_stats(i);
                }
                memset(&s_st, 0, sizeof(s_st));
                eth_raw_clear_stats();
                dbg_log_clear_dropped();
            }
            else
            {
                rsp->status = can_hw_clear_stats(ch);
            }
            break;
        default:
            rsp->status = E2CF_CFG_ENOTSUP;
            break;
    }
}

/*
 * CFG_REQ entry point: token-keyed idempotency cache, execute once, send the
 * response back to the requester's source address.
 */
static void handle_cfg_req(const uint8_t *body, uint16_t remain, const uint8_t *sa)
{
    const e2cf_cfg_body_t *req = (const e2cf_cfg_body_t *)body;
    e2cf_cfg_body_t rsp;
    uint8_t slot;
    cfg_cache_t *cache;

    if (remain < E2CF_CFG_BODY_SIZE)
    {
        s_st.eth_rx_bad++;
        return;
    }
    s_st.cfg_reqs++;

    slot = (req->chan < E2CF_NUM_CHANNELS) ? req->chan : E2CF_NUM_CHANNELS;
    cache = &s_cfg_cache[slot];

    if (cache->valid && (cache->token == req->token))
    {
        rsp = cache->rsp; /* duplicate REQ: idempotent, resend cached RSP */
    }
    else
    {
        cfg_execute(req, &rsp);
        cache->valid = true;
        cache->token = req->token;
        cache->rsp = rsp;
        LOG_DBG(DBG_M_CFG, "CFG op=%u chan=%u token=%u -> status=%u", req->op,
                req->chan, req->token, rsp.status);
    }

    send_immediate(E2CF_MSG_CFG_RSP, &rsp, E2CF_CFG_BODY_SIZE, 1U, sa);
}

/*
 * Peer heartbeat: lock the peer MAC on first contact, refresh the supervision
 * timestamp and clear the safe state.
 */
static void handle_hb(const uint8_t *body, uint16_t remain, const uint8_t *sa)
{
    const e2cf_hb_body_t *hb = (const e2cf_hb_body_t *)body;

    if (remain < E2CF_HB_BODY_SIZE)
    {
        s_st.eth_rx_bad++;
        return;
    }
    (void)hb;

    s_st.hb_rx++;
    if (!s_peer_locked)
    {
        memcpy(s_peer_mac, sa, 6);
        s_peer_locked = true;
        LOG_INF(DBG_M_HB, "peer locked: %02x:%02x:%02x:%02x:%02x:%02x",
                sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
    }
    s_last_hb_rx_cycles = gw_cycles();
    s_ever_saw_hb = true;
    if (s_safe_state)
    {
        s_safe_state = false;
        can_hw_set_tx_gate(false);
        LOG_WRN(DBG_M_HB, "link restored, safe state cleared");
    }
}

/*
 * Frame-sequence accounting (EMAC ISR context). Keeps the legacy seq_gaps
 * event counter (fires on any non-consecutive seq, including mere
 * reordering - the Linux driver assigns the global TX seq across six
 * channels on multiple CPUs, so reorders are routine under concurrent
 * traffic) and additionally maintains a 64-frame sliding window so that
 * losses can be told apart from reorders:
 *   - a hole pushed out of the window still unfilled = confirmed loss;
 *   - an in-window late arrival fills its hole = reorder, not a loss;
 *   - a beyond-window late arrival retro-credits the loss it was already
 *     charged as (the transport never duplicates frames).
 */
static void seq_track(uint16_t seq)
{
    int16_t d;

    if (!s_rx_seq_valid)
    {
        s_rx_seq_valid = true;
        s_rx_seq_expected = (uint16_t)(seq + 1U);
        s_rx_seq_hi = seq;
        s_rx_seq_win = 1U;
        return;
    }

    if (seq != s_rx_seq_expected)
    {
        s_st.seq_gaps++;
    }
    s_rx_seq_expected = (uint16_t)(seq + 1U);

    d = (int16_t)(uint16_t)(seq - s_rx_seq_hi);
    if (d > 0)
    {
        if (d >= 64)
        {
            /* whole window expires: every unfilled hole in it is lost,
             * plus the frames skipped past the new window */
            s_st.seq_lost += 64U - (uint32_t)__builtin_popcountll(s_rx_seq_win)
                             + ((uint32_t)d - 64U);
            s_rx_seq_win = 0U;
        }
        else
        {
            /* the d oldest slots fall off the window's far end; unfilled
             * ones among them are confirmed lost */
            s_st.seq_lost += (uint32_t)d -
                (uint32_t)__builtin_popcountll(s_rx_seq_win >> (64 - d));
            s_rx_seq_win <<= d;
        }
        s_rx_seq_win |= 1U;
        s_rx_seq_hi = seq;
    }
    else if (d < 0)
    {
        if (d >= -63)
        {
            uint64_t bit = 1ULL << (uint32_t)(-d);

            if ((s_rx_seq_win & bit) == 0U)
            {
                s_rx_seq_win |= bit;
                s_st.seq_reorder++;
            }
        }
        else
        {
            s_st.seq_reorder++;
            if (s_st.seq_lost != 0U)
            {
                s_st.seq_lost--;
            }
        }
    }
    /* d == 0: duplicate - cannot happen on this transport, ignore */
}

/*
 * EMAC-IRQ upcall: parse the L2/VLAN and E2CF headers, track RX sequence
 * losses and demux the frame by message type.
 */
void e2cf_core_eth_frame(const uint8_t *frame, uint32_t len)
{
    const uint8_t *payload;
    uint16_t ether_type;
    uint16_t remain;
    const e2cf_hdr_t *hdr;
    uint8_t msg_type;
    /* eth-ingress instant of THIS frame (the host's "t3"); threaded into
     * the TXC of every send it queues so the host can measure residency. */
    uint32_t eth_rx_lo32 = gw_now_ns_lo32();
#if E2CF_PROFILE
    uint32_t eth_t0 = gw_cycles();
#endif

    if (len < (E2CF_L2_HDR_SIZE + E2CF_HDR_SIZE))
    {
        return; /* not ours (runts, other protocols on the wire) */
    }

    ether_type = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
    if (ether_type == E2CF_TPID_8021Q)
    {
        ether_type = (uint16_t)(((uint16_t)frame[16] << 8) | frame[17]);
        payload = &frame[E2CF_L2_HDR_SIZE];
        remain = (uint16_t)(len - E2CF_L2_HDR_SIZE);
    }
    else
    {
        /* Untagged tolerated for bring-up (spec mandates the tag). */
        payload = &frame[14];
        remain = (uint16_t)(len - 14U);
        s_st.eth_rx_untagged++;
    }
    if (ether_type != E2CF_ETHERTYPE)
    {
        return; /* other traffic - the MAC filter lets broadcasts through */
    }

    hdr = (const e2cf_hdr_t *)payload;
    if (E2CF_HDR_VER(hdr->ver_type) != E2CF_VERSION)
    {
        s_st.eth_rx_bad++;
        return;
    }
    s_st.eth_rx_frames++;
    seq_track(hdr->seq);

    msg_type = E2CF_HDR_TYPE(hdr->ver_type);
    payload += E2CF_HDR_SIZE;
    remain -= E2CF_HDR_SIZE;

    switch (msg_type)
    {
        case E2CF_MSG_DATA:
            handle_data_msg(payload, remain, hdr->count, eth_rx_lo32);
            break;
        case E2CF_MSG_CFG_REQ:
            handle_cfg_req(payload, remain, &frame[6]); /* RSP back to SA */
            break;
        case E2CF_MSG_HB:
            handle_hb(payload, remain, &frame[6]);
            break;
        default:
            /* TXC/EVT/TIME are MCU->Linux only; tolerate and skip. */
            break;
    }
#if E2CF_PROFILE
    /* Full EMAC-ISR cost for a valid E2CF frame: header parse + demux +
     * (for DATA) per-record can_hw_submit_tx into the sw_txfifo. Non-E2CF
     * frames returned early above and are deliberately not sampled. */
    gw_prof_add(PROF_ETH_FRAME, gw_cycles() - eth_t0);
#endif
}

/* ---------------------------------------------------------------------- */
/* Periodic work (main loop)                                               */
/* ---------------------------------------------------------------------- */

/* Build and send one heartbeat body reflecting the current gateway state. */
static void send_hb(void)
{
    e2cf_hb_body_t hb;

    memset(&hb, 0, sizeof(hb));
    hb.state = s_safe_state ? E2CF_HB_STATE_FAULT
                           : (s_peer_locked ? E2CF_HB_STATE_READY : E2CF_HB_STATE_INIT);
    hb.nchan = E2CF_NUM_CHANNELS;
    hb.uptime_s = (uint32_t)(gw_now_ns() / 1000000000ULL);
    send_immediate(E2CF_MSG_HB, &hb, E2CF_HB_BODY_SIZE, 1U, peer_da());
    s_st.hb_tx++;
}

/* Snapshot all channels' EVT records into one periodic EVT frame. */
static void send_evt_periodic(void)
{
    e2cf_evt_rec_t evts[E2CF_NUM_CHANNELS];

    for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
    {
        can_hw_get_evt(i, &evts[i]);
    }
    send_immediate(E2CF_MSG_EVT, evts, (uint16_t)sizeof(evts), E2CF_NUM_CHANNELS, peer_da());
}

/* Send the 1 Hz TIME message carrying the full 64-bit gateway clock. */
static void send_time(void)
{
    e2cf_time_body_t tm;

    memset(&tm, 0, sizeof(tm));
    tm.full_time_ns = gw_now_ns();
    tm.flags = 0U; /* not 1588-synced in v1 */
    send_immediate(E2CF_MSG_TIME, &tm, E2CF_TIME_BODY_SIZE, 1U, peer_da());
}

/* STATS push (spec §4.9): full counter snapshot, 1 Hz, PCP 2. Superloop
 * context; plain u32 reads of ISR-written counters give the same
 * per-counter-atomic snapshot the UART dump has. */
static void send_stats(void)
{
    uint8_t body[E2CF_STATS_BODY_SIZE];
    e2cf_stats_hdr_t *hdr = (e2cf_stats_hdr_t *)&body[0];
    e2cf_stats_global_t *g = (e2cf_stats_global_t *)&body[E2CF_STATS_HDR_SIZE];
    eth_raw_stats_t es;

    memset(body, 0, sizeof(body));

    hdr->layout_rev = E2CF_STATS_LAYOUT_REV;
    hdr->nchan = E2CF_NUM_CHANNELS;
    hdr->global_len = E2CF_STATS_GLOBAL_SIZE;
    hdr->chan_len = E2CF_STATS_CHAN_SIZE;
    hdr->gw_time_ns = gw_now_ns();

    eth_raw_get_stats(&es);
    g->eth_rx_frames = s_st.eth_rx_frames;
    g->eth_rx_bad = s_st.eth_rx_bad;
    g->eth_rx_untagged = s_st.eth_rx_untagged;
    g->seq_gaps = s_st.seq_gaps;
    g->seq_lost = s_st.seq_lost;
    g->seq_reorder = s_st.seq_reorder;
    g->loop_per_s = s_loop_iters - s_loop_last;
    s_loop_last = s_loop_iters;
    g->data_rx_recs = s_st.data_rx_recs;
    g->data_rx_rejects = s_st.data_rx_rejects;
    g->data_tx_recs = s_st.data_tx_recs;
    g->txc_recs = s_st.txc_recs;
    g->rx_gated_drops = s_st.rx_gated_drops;
    g->face_starved = s_st.face_starved;
    g->frames_sent = s_st.frames_sent;
    g->send_fail = s_st.send_fail;
    g->cfg_reqs = s_st.cfg_reqs;
    g->hb_rx = s_st.hb_rx;
    g->hb_tx = s_st.hb_tx;
    g->emac_rx_frames = es.rx_frames;
    g->emac_rx_errors = es.rx_errors;
    g->emac_rx_drops = es.rx_drops;
    g->emac_tx_frames = es.tx_frames;
    g->emac_tx_busy = es.tx_busy;
    g->dbg_log_dropped = dbg_log_dropped();
    g->flags = (uint8_t)((s_peer_locked ? E2CF_STATS_F_PEER_LOCKED : 0U) |
                         (s_safe_state ? E2CF_STATS_F_SAFE_STATE : 0U) |
                         (eth_raw_link_up() ? E2CF_STATS_F_ETH_LINK : 0U));

    for (uint8_t i = 0U; i < E2CF_NUM_CHANNELS; i++)
    {
        e2cf_stats_chan_t *c =
            (e2cf_stats_chan_t *)&body[E2CF_STATS_HDR_SIZE + E2CF_STATS_GLOBAL_SIZE +
                                       ((uint32_t)i * E2CF_STATS_CHAN_SIZE)];
        const can_hw_stats_t *hs = can_hw_stats(i);
        e2cf_evt_rec_t evt;

        can_hw_get_evt(i, &evt);
        if (hs != NULL)
        {
            c->rx_frames = hs->rx_frames;
            c->rx_ovf = hs->rx_ovf;
            c->tx_frames = hs->tx_frames;
            c->tx_rejected = hs->tx_rejected;
            c->tx_timeout = hs->tx_timeout;
            c->fifo_hwm = hs->fifo_hwm;
            c->irq_count = hs->irq_count;
            c->bus_off_cnt = hs->bus_off_cnt;
            c->arb_lost_cnt = hs->arb_lost_cnt;
        }
        c->state = evt.state;
        c->tec = evt.tec;
        c->rec = evt.rec;
        c->flags = can_hw_is_running(i) ? E2CF_STATS_CHAN_F_RUNNING : 0U;
        c->rx_ovf_cnt = evt.rx_ovf_cnt;
    }

    send_immediate(E2CF_MSG_STATS, body, E2CF_STATS_BODY_SIZE, 1U, peer_da());
}

/*
 * Reset all protocol state: faces, aggregation contexts, peer/sequence
 * tracking and statistics.
 */
bool e2cf_core_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    memset(s_cfg_cache, 0, sizeof(s_cfg_cache));
    memset((void *)s_face_state, 0, sizeof(s_face_state));
    agg_reset(&s_agg_data, E2CF_MSG_DATA);
    agg_reset(&s_agg_txc, E2CF_MSG_TXC);
    s_tx_seq = 0U;
    s_peer_locked = false;
    s_ever_saw_hb = false;
    s_safe_state = false;
    s_rx_seq_valid = false;

    LOG_INF(DBG_M_PROTO, "e2cf core: proto v%u, win=%u, T_agg=%uus", E2CF_VERSION,
            E2CF_WIN_DEPTH, E2CF_T_AGG_US);
    return true;
}

/*
 * Superloop tick: flush aggregation frames past their T_agg deadline, run the
 * HB/EVT/TIME/STATS cadences and supervise the peer heartbeat timeout.
 */
void e2cf_core_poll(void)
{
    static uint32_t s_last_hb_tx;
    static uint32_t s_last_time;
    static uint32_t s_last_stats;
    static bool s_stats_seeded;
    uint32_t now = gw_cycles();
    uint32_t per_us = gw_cycles_per_us();
    uint32_t primask;

    /* CPU headroom gauge: superloop pass rate, exported via STATS as
     * loop_per_s (drops as ISR data-plane load grows). */
    s_loop_iters++;

    /* T_agg deadline flush (sub-us check resolution from the superloop). */
    primask = __get_PRIMASK();
    __disable_irq();
    if ((s_agg_data.face >= 0) && ((int32_t)(now - s_agg_data.deadline) >= 0))
    {
        agg_flush(&s_agg_data);
    }
    if ((s_agg_txc.face >= 0) && ((int32_t)(now - s_agg_txc.deadline) >= 0))
    {
        agg_flush(&s_agg_txc);
    }
    face_reclaim();
    __set_PRIMASK(primask);

    /* HB TX + periodic EVT every 100 ms; TIME every 1 s. */
    if ((now - s_last_hb_tx) >= (E2CF_HB_PERIOD_MS * 1000U * per_us))
    {
        s_last_hb_tx = now;
        if (eth_raw_link_up())
        {
            send_hb();
            if (s_peer_locked)
            {
                send_evt_periodic();
            }
        }
    }
    if ((now - s_last_time) >= (1000U * 1000U * per_us))
    {
        s_last_time = now;
        if (s_peer_locked)
        {
            send_time();
        }
    }
    /* STATS every 1 s, phase-offset 500 ms from TIME so the worst-case
     * simultaneous face demand (HB+EVT+TIME+STATS on a coinciding tick)
     * never asks for 4 of the 6 faces at once. */
    if (!s_stats_seeded)
    {
        s_stats_seeded = true;
        s_last_stats = now - (500U * 1000U * per_us);
    }
    if ((now - s_last_stats) >= (1000U * 1000U * per_us))
    {
        s_last_stats = now;
        if (s_peer_locked)
        {
            send_stats();
        }
    }

    /* Link supervision: 500 ms without peer HB -> safe state (spec §4.8).
     * Not armed until the first HB ever (bench bring-up without a peer).
     * Runs IRQ-masked with a FRESH clock sample: handle_hb() (EMAC ISR)
     * stamps s_last_hb_rx_cycles at any time, and comparing it against the
     * stale poll-entry 'now' underflowed the unsigned delta to ~2^32
     * whenever an HB landed mid-poll - falsely declaring a timeout at the
     * very moment the link proved alive (rapid SAFE STATE flapping that
     * gated all CAN TX). Masking also keeps a genuine timeout decision
     * from racing the ISR's safe-state clear. */
    if (s_ever_saw_hb && !s_safe_state)
    {
        bool expired;

        primask = __get_PRIMASK();
        __disable_irq();
        expired = ((gw_cycles() - s_last_hb_rx_cycles) >=
                   (E2CF_HB_TIMEOUT_MS * 1000U * per_us));
        if (expired)
        {
            s_safe_state = true;
            can_hw_set_tx_gate(true);
        }
        __set_PRIMASK(primask);
        if (expired)
        {
            LOG_ERR(DBG_M_HB, "peer HB timeout (%ums): SAFE STATE", E2CF_HB_TIMEOUT_MS);
        }
    }
}

bool e2cf_core_link_ready(void)
{
    return s_peer_locked && !s_safe_state;
}

void e2cf_core_dump_stats(void)
{
    LOG_INF(DBG_M_STATS,
            "E2CF rx=%lu bad=%lu untag=%lu gaps=%lu lost=%lu reord=%lu | up=%lu txc=%lu | dn=%lu rej=%lu",
            (unsigned long)s_st.eth_rx_frames, (unsigned long)s_st.eth_rx_bad,
            (unsigned long)s_st.eth_rx_untagged, (unsigned long)s_st.seq_gaps,
            (unsigned long)s_st.seq_lost, (unsigned long)s_st.seq_reorder,
            (unsigned long)s_st.data_tx_recs, (unsigned long)s_st.txc_recs,
            (unsigned long)s_st.data_rx_recs, (unsigned long)s_st.data_rx_rejects);
    LOG_INF(DBG_M_STATS,
            "E2CF sent=%lu sfail=%lu starv=%lu gated=%lu cfg=%lu hbrx=%lu hbtx=%lu peer=%u safe=%u",
            (unsigned long)s_st.frames_sent, (unsigned long)s_st.send_fail,
            (unsigned long)s_st.face_starved, (unsigned long)s_st.rx_gated_drops,
            (unsigned long)s_st.cfg_reqs, (unsigned long)s_st.hb_rx,
            (unsigned long)s_st.hb_tx, (unsigned)s_peer_locked, (unsigned)s_safe_state);
}
