// SPDX-License-Identifier: GPL-2.0-only
/*
 * eth2can.c - E2CF Ethernet-to-CANFD gateway driver
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * Exposes the 6 CANFD channels of the MCXE31B gateway as standard SocketCAN
 * network devices (eth2can0..5).
 *
 * Transport: a packet_type hook on an existing ethernet interface.
 * dev_add_pack(ETH 0x88B5) intercepts ONLY E2CF frames; all other traffic
 * flows through the regular stack untouched, and the NIC keeps its stock
 * driver. TX uses dev_queue_xmit().
 *
 * Architecture model: gs_usb (multi-channel mux, echo-id window flow
 * control); wire format: see e2cf_proto.h and docs/e2cf-protocol-spec.md.
 *
 * Usage:
 *   insmod eth2can.ko ifname=eth0 [vid=100] [peer=aa:bb:cc:dd:ee:ff]
 *   ip link set eth2can0 type can bitrate 1000000 dbitrate 5000000 fd on
 *   ip link set eth2can0 up
 */
#include <linux/bitops.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "e2cf_proto.h"

/* Wire-layout guards for the STATS body (spec §4.9). */
static_assert(sizeof(e2cf_stats_hdr_t) == E2CF_STATS_HDR_SIZE);
static_assert(sizeof(e2cf_stats_global_t) == E2CF_STATS_GLOBAL_SIZE);
static_assert(sizeof(e2cf_stats_chan_t) == E2CF_STATS_CHAN_SIZE);

#define E2CF_DRV_NAME "eth2can"

/* Receiver-side liveness for the TXC window (spec §7): a slot whose TXC has
 * not arrived after this long is reclaimed locally. 10x the firmware's
 * worst-case TXC latency (100 ms stuck-TX watchdog + aggregation). */
#define E2CF_TXC_SLOT_TIMEOUT_MS 1000

static char *ifname = "eth0";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "underlying ethernet interface (default eth0)");

static int vid = -1;
module_param(vid, int, 0444);
MODULE_PARM_DESC(vid, "802.1Q VID for E2CF frames; -1 = untagged (default, phase-1 bring-up)");

static char *peer = "";
module_param(peer, charp, 0444);
MODULE_PARM_DESC(peer, "static peer MAC (default: learned from gateway HB)");

/* ------------------------------------------------------------------ */

struct e2cf_dev;

struct e2cf_chan {
	struct can_priv can; /* must be first */
	struct e2cf_dev *edev;
	struct net_device *ndev;
	u8 ch;
	spinlock_t tx_lock;       /* echo window state */
	unsigned long echo_busy;  /* bitmap of in-flight echo slots */
	unsigned long echo_ts[E2CF_WIN_DEPTH]; /* claim time, jiffies */
	ktime_t echo_t2[E2CF_WIN_DEPTH];       /* t2: eth-egress-from-Linux instant */
	bool recovering;          /* reboot recovery in progress: TXC paths
				   * must not wake the quiesced queue */

	/* Last gateway-reported error gauges (EVT 100 ms / STATS 1 Hz).
	 * Single softirq writer; lock-free READ_ONCE readers. */
	u8 gw_tec, gw_rec;
	u16 gw_arb_lost, gw_rx_ovf;
	u32 gw_err_flags;
	u32 stat_txc_tmo_reclaims; /* slots reclaimed by the 1 s timeout */
	u32 stat_txc_err;          /* TXCs that arrived with status != OK */
	u32 stat_txc_rej[8];       /* non-OK TXCs split by E2CF_TXC_* status */
};

struct e2cf_dev {
	struct net_device *lower;
	struct packet_type pt;
	struct e2cf_chan *chan[E2CF_NUM_CHANNELS];

	u8 peer_mac[ETH_ALEN];
	bool peer_locked;
	bool gw_alive;
	bool gw_ever_alive;       /* distinguishes first contact from a resume */
	bool gw_uptime_valid;
	u32 gw_uptime_s;          /* last HB uptime - regression = reboot */
	unsigned long last_hb_rx; /* jiffies */
	struct delayed_work hb_work;
	struct work_struct reboot_work; /* gateway-reboot recovery, see below */

	u16 tx_seq; /* protected by seq_lock */
	spinlock_t seq_lock;

	/* one CFG transaction at a time (netlink/open paths are sleepable) */
	struct mutex cfg_lock;
	struct completion cfg_done;
	u8 cfg_token;
	bool cfg_waiting;
	e2cf_cfg_body_t cfg_rsp;

	u32 stat_rx_frames;
	u32 stat_rx_bad;
	u32 stat_seq_gaps;
	u32 stat_cfg_retries;
	u32 stat_cfg_timeouts;
	u16 rx_seq_expected;
	bool rx_seq_valid;

	/* Frame-sequence loss accounting (e2cf_seq_track). rx_lock makes the
	 * tracker safe when the lower NIC is multi-queue and e2cf_rcv runs
	 * concurrently on several CPUs; readers (debugfs/ethtool) stay
	 * lock-free on the plain u32 counters as elsewhere. */
	spinlock_t rx_lock;
	u16 rx_seq_hi;        /* highest sequence number seen */
	u64 rx_seq_win;       /* bit i = frame (rx_seq_hi - i) arrived */
	u32 stat_seq_lost;    /* confirmed lost (hole expired unfilled) */
	u32 stat_seq_reorder; /* arrived out of order (filled a hole) */
	u32 stat_rx_nomem;    /* skb_share_check failed (GFP_ATOMIC) */
	u32 stat_rx_trunc;    /* pskb_may_pull failed (truncated frame) */
	atomic_t stat_tx_cn;  /* lower qdisc congestion (NET_XMIT_CN) */

	/* Gateway STATS push cache (spec §4.9). Writer: softirq e2cf_rx_stats;
	 * readers: ethtool/debugfs (process context) copy out under the lock. */
	spinlock_t stats_lock;
	bool gw_stats_valid;
	unsigned long gw_stats_rx; /* jiffies of last STATS frame */
	e2cf_stats_hdr_t gw_stats_hdr;
	e2cf_stats_global_t gw_stats_g;
	e2cf_stats_chan_t gw_stats_ch[E2CF_NUM_CHANNELS];

	/* Gateway TIME cache (1 Hz) for clock correlation. */
	bool gw_time_valid;
	u64 gw_time_ns;        /* gateway monotonic clock */
	u32 gw_time_flags;     /* bit0 = 1588-synced */
	u64 gw_time_local_ns;  /* ktime_get_ns() at receive (debugfs only) */

	struct dentry *dbg_dir;
};

static struct e2cf_dev *g_edev;

static const u8 e2cf_hb_mcast[ETH_ALEN] = E2CF_HB_MCAST_DA;

/*
 * Bit timing limits mirroring the MCXE31B FlexCAN @ 80 MHz PE clock
 * (CBT/EDCBT field widths; see can_hw.c timing_from_raw()).
 */
static const struct can_bittiming_const e2cf_nom_bittiming_const = {
	.name = E2CF_DRV_NAME,
	.tseg1_min = 2,
	.tseg1_max = 96,	/* ENCBT[NTSEG1] allows 256; 96 is the sane CiA range */
	.tseg2_min = 2,
	.tseg2_max = 32,	/* ENCBT[NTSEG2] allows 128 */
	.sjw_max = 32,
	.brp_min = 1,
	.brp_max = 1024,	/* EPRS prescaler field; oddball brp/tq combos that
				 * would trip the MCU SDK's init assert are rejected
				 * by the firmware with CFG EINVAL at open time */
	.brp_inc = 1,
};

static const struct can_bittiming_const e2cf_dat_bittiming_const = {
	.name = E2CF_DRV_NAME,
	.tseg1_min = 2,
	.tseg1_max = 32,	/* EDCBT[DTSEG1] is 5 bits = tseg1-1 */
	.tseg2_min = 2,
	.tseg2_max = 16,	/* EDCBT[DTSEG2] is 4 bits = tseg2-1 */
	.sjw_max = 16,
	.brp_min = 1,
	.brp_max = 255,		/* wire limit: e2cf_proto.h dat_brp is u8 */
	.brp_inc = 1,
};

/* ------------------------------------------------------------------ */
/* TX frame construction                                               */
/* ------------------------------------------------------------------ */

static u16 e2cf_next_seq(struct e2cf_dev *edev)
{
	unsigned long flags;
	u16 seq;

	spin_lock_irqsave(&edev->seq_lock, flags);
	seq = edev->tx_seq++;
	spin_unlock_irqrestore(&edev->seq_lock, flags);
	return seq;
}

/*
 * Allocate an skb on the lower device and write L2 + E2CF headers.
 * Returns the skb with *body pointing at the first record/body byte.
 */
static struct sk_buff *e2cf_build_skb(struct e2cf_dev *edev, u8 msg_type,
				      u16 body_len, u8 count, u8 **body)
{
	bool tagged = (vid >= 0);
	u16 l2_len = tagged ? E2CF_L2_HDR_SIZE : (ETH_HLEN);
	u16 frame_len = l2_len + E2CF_HDR_SIZE + body_len;
	struct sk_buff *skb;
	u8 *p;
	e2cf_hdr_t *hdr;
	const u8 *da = edev->peer_locked ? edev->peer_mac : e2cf_hb_mcast;

	skb = netdev_alloc_skb(edev->lower, frame_len + 2);
	if (!skb)
		return NULL;

	p = skb_put(skb, frame_len);
	memcpy(&p[0], da, ETH_ALEN);
	memcpy(&p[6], edev->lower->dev_addr, ETH_ALEN);
	if (tagged) {
		u8 pcp = (msg_type == E2CF_MSG_CFG_REQ || msg_type == E2CF_MSG_CFG_RSP)
				 ? E2CF_PCP_CFG : E2CF_PCP_DATA;
		u16 tci = (pcp << 13) | (vid & 0x0FFF);

		p[12] = E2CF_TPID_8021Q >> 8;
		p[13] = E2CF_TPID_8021Q & 0xFF;
		p[14] = tci >> 8;
		p[15] = tci & 0xFF;
		p[16] = E2CF_ETHERTYPE >> 8;
		p[17] = E2CF_ETHERTYPE & 0xFF;
		skb->protocol = htons(ETH_P_8021Q);
	} else {
		p[12] = E2CF_ETHERTYPE >> 8;
		p[13] = E2CF_ETHERTYPE & 0xFF;
		skb->protocol = htons(E2CF_ETHERTYPE);
	}

	hdr = (e2cf_hdr_t *)&p[l2_len];
	hdr->ver_type = E2CF_VER_TYPE(E2CF_VERSION, msg_type);
	hdr->count = count;
	hdr->seq = e2cf_next_seq(edev);
	hdr->ts_base = (u32)ktime_get_ns();

	skb->dev = edev->lower;
	skb_reset_network_header(skb);
	*body = &p[l2_len + E2CF_HDR_SIZE];
	return skb;
}

/* Hand a built E2CF frame to the lower NIC. Congestion (NET_XMIT_CN)
 * still counts as accepted - the lower qdisc may have dropped a frame
 * (possibly this one), so the event is counted for diagnosis; only a
 * hard drop reports -ENETDOWN. */
static int e2cf_send(struct e2cf_dev *edev, struct sk_buff *skb)
{
	int ret = dev_queue_xmit(skb);

	if (ret == NET_XMIT_CN)
		atomic_inc(&edev->stat_tx_cn);
	return (ret == NET_XMIT_SUCCESS || ret == NET_XMIT_CN) ? 0 : -ENETDOWN;
}

/* ------------------------------------------------------------------ */
/* SocketCAN TX path (Linux -> MCU DATA, direct send: T_agg = 0)       */
/* ------------------------------------------------------------------ */

static netdev_tx_t e2cf_ndo_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct e2cf_chan *chan = netdev_priv(ndev);
	struct e2cf_dev *edev = chan->edev;
	struct canfd_frame *cfd;
	bool fd;
	u8 len;
	u16 rec_size;
	struct sk_buff *eskb;
	e2cf_data_rec_t *rec;
	unsigned long flags;
	int echo_id;
	u8 *body;

	if (can_dropped_invalid_skb(ndev, skb))
		return NETDEV_TX_OK;

	cfd = (struct canfd_frame *)skb->data;
	fd = can_is_canfd_skb(skb);
	len = fd ? cfd->len : ((struct can_frame *)cfd)->len;
	rec_size = E2CF_DATA_REC_SIZE(len);

	spin_lock_irqsave(&chan->tx_lock, flags);
	echo_id = find_first_zero_bit(&chan->echo_busy, E2CF_WIN_DEPTH);
	if (echo_id >= E2CF_WIN_DEPTH) {
		/* window full - stop the queue until a TXC frees a slot */
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&chan->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	set_bit(echo_id, &chan->echo_busy);
	chan->echo_ts[echo_id] = jiffies;
	if (find_first_zero_bit(&chan->echo_busy, E2CF_WIN_DEPTH) >= E2CF_WIN_DEPTH)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&chan->tx_lock, flags);

	eskb = e2cf_build_skb(edev, E2CF_MSG_DATA, rec_size, 1, &body);
	if (!eskb) {
		spin_lock_irqsave(&chan->tx_lock, flags);
		clear_bit(echo_id, &chan->echo_busy);
		spin_unlock_irqrestore(&chan->tx_lock, flags);
		netif_wake_queue(ndev);
		ndev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	rec = (e2cf_data_rec_t *)body;
	rec->can_id = cfd->can_id; /* SocketCAN bit layout = wire layout */
	rec->len = len;
	rec->flags = 0;
	if (fd) {
		rec->flags |= E2CF_DATA_FLAG_FDF;
		if (cfd->flags & CANFD_BRS)
			rec->flags |= E2CF_DATA_FLAG_BRS;
	}
	rec->chan = chan->ch;
	rec->tag = echo_id;
	if (len) {
		if (cfd->can_id & CAN_RTR_FLAG) {
			/* remote frame: no data section on the bus; keep the
			 * wire record deterministic (no skb memory leak) */
			memset(&body[E2CF_DATA_REC_HEAD_SIZE], 0,
			       E2CF_CEIL4(len));
		} else {
			memcpy(&body[E2CF_DATA_REC_HEAD_SIZE], cfd->data, len);
			if (len & 3)
				memset(&body[E2CF_DATA_REC_HEAD_SIZE + len], 0,
				       4 - (len & 3));
		}
	}

	/* Record the "t2" eth-egress-from-Linux instant (CLOCK_REALTIME) for
	 * this slot; applied to the echo skb's software timestamp when the TXC
	 * loops it back (see e2cf_rx_txc), so the app reads t2 alongside the
	 * gateway hardware stamps. */
	chan->echo_t2[echo_id] = ktime_get_real();
	/* Echo skb: looped back to local listeners when the TXC confirms. */
	can_put_echo_skb(skb, ndev, echo_id, 0);

	if (e2cf_send(edev, eskb)) {
		spin_lock_irqsave(&chan->tx_lock, flags);
		can_free_echo_skb(ndev, echo_id, NULL);
		clear_bit(echo_id, &chan->echo_busy);
		spin_unlock_irqrestore(&chan->tx_lock, flags);
		netif_wake_queue(ndev);
		ndev->stats.tx_errors++;
	}
	return NETDEV_TX_OK;
}

/* ------------------------------------------------------------------ */
/* CFG transactions (request/response with token, 10 ms x3 retries)    */
/* ------------------------------------------------------------------ */

static int e2cf_cfg_xact(struct e2cf_dev *edev, e2cf_cfg_body_t *req,
			 e2cf_cfg_body_t *rsp)
{
	int retry, ret = -ETIMEDOUT;

	mutex_lock(&edev->cfg_lock);
	req->token = ++edev->cfg_token;
	req->status = 0;

	for (retry = 0; retry < E2CF_CFG_RETRIES; retry++) {
		struct sk_buff *skb;
		u8 *body;

		reinit_completion(&edev->cfg_done);
		edev->cfg_waiting = true;

		skb = e2cf_build_skb(edev, E2CF_MSG_CFG_REQ, E2CF_CFG_BODY_SIZE, 1, &body);
		if (!skb) {
			ret = -ENOMEM;
			break;
		}
		memcpy(body, req, E2CF_CFG_BODY_SIZE);
		if (e2cf_send(edev, skb)) {
			ret = -ENETDOWN;
			break;
		}

		if (wait_for_completion_timeout(&edev->cfg_done,
						msecs_to_jiffies(E2CF_CFG_TIMEOUT_MS))) {
			*rsp = edev->cfg_rsp;
			ret = (rsp->status == E2CF_CFG_OK) ? 0 : -EINVAL;
			break;
		}
	}
	edev->cfg_waiting = false;
	if (retry > 0)
		edev->stat_cfg_retries += retry; /* actual resends performed */
	mutex_unlock(&edev->cfg_lock);

	if (ret == -ETIMEDOUT) {
		edev->stat_cfg_timeouts++;
		netdev_warn(edev->lower, "e2cf: CFG op %u timed out (gateway down?)\n",
			    req->op);
	}
	return ret;
}

/* Push the channel's netlink-configured nominal/data bit timing and
 * ctrlmode flags to the gateway via CFG SET_BITRATE. */
static int e2cf_chan_set_bitrate(struct e2cf_chan *chan)
{
	const struct can_bittiming *nbt = &chan->can.bittiming;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	const struct can_bittiming *dbt = &chan->can.fd.data_bittiming;
#else
	const struct can_bittiming *dbt = &chan->can.data_bittiming;
#endif
	bool fd = (chan->can.ctrlmode & CAN_CTRLMODE_FD) != 0;
	e2cf_cfg_body_t req = { 0 }, rsp;

	req.op = E2CF_CFG_OP_SET_BITRATE;
	req.chan = chan->ch;
	req.u.bitrate.nom_brp = nbt->brp;
	req.u.bitrate.nom_tseg1 = nbt->prop_seg + nbt->phase_seg1;
	req.u.bitrate.nom_tseg2 = nbt->phase_seg2;
	req.u.bitrate.nom_sjw = nbt->sjw;
	if (fd) {
		req.u.bitrate.dat_brp = dbt->brp;
		req.u.bitrate.dat_tseg1 = dbt->prop_seg + dbt->phase_seg1;
		req.u.bitrate.dat_tseg2 = dbt->phase_seg2;
		req.u.bitrate.dat_sjw = dbt->sjw;
		req.u.bitrate.mode_flags |= E2CF_MODE_FD;
	}
	if (chan->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		req.u.bitrate.mode_flags |= E2CF_MODE_LISTENONLY;
	if (chan->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		req.u.bitrate.mode_flags |= E2CF_MODE_LOOPBACK;
	/* No E2CF_MODE_NONISO mapping: v1 firmware is ISO-only and rejects the
	 * flag with ENOTSUP (CAN_CTRLMODE_FD_NON_ISO is not advertised). */

	return e2cf_cfg_xact(chan->edev, &req, &rsp);
}

/* Issue a parameter-less per-channel CFG operation (START/STOP/...). */
static int e2cf_chan_simple_op(struct e2cf_chan *chan, u8 op)
{
	e2cf_cfg_body_t req = { 0 }, rsp;

	req.op = op;
	req.chan = chan->ch;
	return e2cf_cfg_xact(chan->edev, &req, &rsp);
}

/* STOP -> SET_BITRATE -> START against the gateway. Used by ndo_open and by
 * gateway-reboot recovery. Sleeps on CFG responses; callers hold rtnl. */
static int e2cf_chan_configure(struct e2cf_chan *chan)
{
	int err;

	(void)e2cf_chan_simple_op(chan, E2CF_CFG_OP_STOP); /* clean slate */
	err = e2cf_chan_set_bitrate(chan);
	if (!err)
		err = e2cf_chan_simple_op(chan, E2CF_CFG_OP_START);
	return err;
}

/* ------------------------------------------------------------------ */
/* SocketCAN open/stop                                                 */
/* ------------------------------------------------------------------ */

static int e2cf_ndo_open(struct net_device *ndev)
{
	struct e2cf_chan *chan = netdev_priv(ndev);
	unsigned long flags;
	int err;

	if (!READ_ONCE(chan->edev->gw_alive))
		return -ENETDOWN;

	err = open_candev(ndev);
	if (err)
		return err;

	err = e2cf_chan_configure(chan);
	if (err) {
		close_candev(ndev);
		return err;
	}

	spin_lock_irqsave(&chan->tx_lock, flags);
	chan->echo_busy = 0; /* stale late TXCs race this - close_candev
			      * already flushed the echo skbs */
	chan->recovering = false;
	spin_unlock_irqrestore(&chan->tx_lock, flags);
	chan->can.state = CAN_STATE_ERROR_ACTIVE;
	netif_start_queue(ndev);
	return 0;
}

/* ndo_stop: stop the TX queue, send CFG STOP to the gateway and close
 * the candev. Called under rtnl. */
static int e2cf_ndo_stop(struct net_device *ndev)
{
	struct e2cf_chan *chan = netdev_priv(ndev);

	netif_stop_queue(ndev);
	(void)e2cf_chan_simple_op(chan, E2CF_CFG_OP_STOP);
	chan->can.state = CAN_STATE_STOPPED;
	close_candev(ndev);
	return 0;
}

static const struct net_device_ops e2cf_netdev_ops = {
	.ndo_open = e2cf_ndo_open,
	.ndo_stop = e2cf_ndo_stop,
	.ndo_start_xmit = e2cf_ndo_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

/* ------------------------------------------------------------------ */
/* ethtool -S / berr counters (gateway stats cached from EVT + STATS)  */
/* ------------------------------------------------------------------ */

/* `ip -d link show` berr-counter, from the gateway's last reported TEC/REC
 * (EVT every 100 ms, STATS every 1 s). */
static int e2cf_get_berr_counter(const struct net_device *ndev,
				 struct can_berr_counter *bec)
{
	const struct e2cf_chan *chan = netdev_priv(ndev);

	bec->txerr = READ_ONCE(chan->gw_tec);
	bec->rxerr = READ_ONCE(chan->gw_rec);
	return 0;
}

/* Prefixes: drv_ = this driver per channel, drvg_ = this driver global
 * (same value on all 6 netdevs), gw_ = MCU per channel, gwg_ = MCU global.
 * Order must match e2cf_get_ethtool_stats(). */
static const char e2cf_gstrings[][ETH_GSTRING_LEN] = {
	"drv_txc_tmo_reclaims",
	"drv_txc_err",
	"gw_rx_frames",
	"gw_rx_ovf",
	"gw_tx_frames",
	"gw_tx_rejected",
	"gw_tx_timeout",
	"gw_fifo_hwm",
	"gw_irq_count",
	"gw_bus_off",
	"gw_arb_lost",
	"gw_rx_ovf_cnt",
	"gwg_eth_rx_frames",
	"gwg_eth_rx_bad",
	"gwg_eth_rx_untagged",
	"gwg_seq_gaps",
	"gwg_seq_lost",
	"gwg_seq_reorder",
	"gwg_data_rx_recs",
	"gwg_data_rx_rejects",
	"gwg_data_tx_recs",
	"gwg_txc_recs",
	"gwg_rx_gated_drops",
	"gwg_face_starved",
	"gwg_frames_sent",
	"gwg_send_fail",
	"gwg_cfg_reqs",
	"gwg_hb_rx",
	"gwg_hb_tx",
	"gwg_emac_rx_frames",
	"gwg_emac_rx_errors",
	"gwg_emac_rx_drops",
	"gwg_emac_tx_frames",
	"gwg_emac_tx_busy",
	"gwg_dbg_log_dropped",
	"gwg_loop_per_s",
	"drvg_rx_frames",
	"drvg_rx_bad",
	"drvg_seq_gaps",
	"drvg_seq_lost",
	"drvg_seq_reorder",
	"drvg_rx_nomem",
	"drvg_rx_trunc",
	"drvg_tx_cn",
	"drvg_cfg_retries",
	"drvg_cfg_timeouts",
};

/* ethtool -S string table; order must match e2cf_get_ethtool_stats(). */
static void e2cf_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS)
		memcpy(data, e2cf_gstrings, sizeof(e2cf_gstrings));
}

/* ethtool statistics count for ETH_SS_STATS. */
static int e2cf_get_sset_count(struct net_device *ndev, int sset)
{
	return (sset == ETH_SS_STATS) ? ARRAY_SIZE(e2cf_gstrings) : -EOPNOTSUPP;
}

/* Reads the STATS cache only (zeros until the first 1 Hz push from the
 * gateway lands) - never a wire round trip. */
static void e2cf_get_ethtool_stats(struct net_device *ndev,
				   struct ethtool_stats *stats, u64 *data)
{
	struct e2cf_chan *chan = netdev_priv(ndev);
	struct e2cf_dev *edev = chan->edev;
	e2cf_stats_chan_t cs;
	e2cf_stats_global_t g;
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&edev->stats_lock, flags);
	cs = edev->gw_stats_ch[chan->ch];
	g = edev->gw_stats_g;
	spin_unlock_irqrestore(&edev->stats_lock, flags);

	data[n++] = chan->stat_txc_tmo_reclaims;
	data[n++] = chan->stat_txc_err;
	data[n++] = cs.rx_frames;
	data[n++] = cs.rx_ovf;
	data[n++] = cs.tx_frames;
	data[n++] = cs.tx_rejected;
	data[n++] = cs.tx_timeout;
	data[n++] = cs.fifo_hwm;
	data[n++] = cs.irq_count;
	data[n++] = cs.bus_off_cnt;
	data[n++] = cs.arb_lost_cnt;
	data[n++] = cs.rx_ovf_cnt;
	data[n++] = g.eth_rx_frames;
	data[n++] = g.eth_rx_bad;
	data[n++] = g.eth_rx_untagged;
	data[n++] = g.seq_gaps;
	data[n++] = g.seq_lost;
	data[n++] = g.seq_reorder;
	data[n++] = g.data_rx_recs;
	data[n++] = g.data_rx_rejects;
	data[n++] = g.data_tx_recs;
	data[n++] = g.txc_recs;
	data[n++] = g.rx_gated_drops;
	data[n++] = g.face_starved;
	data[n++] = g.frames_sent;
	data[n++] = g.send_fail;
	data[n++] = g.cfg_reqs;
	data[n++] = g.hb_rx;
	data[n++] = g.hb_tx;
	data[n++] = g.emac_rx_frames;
	data[n++] = g.emac_rx_errors;
	data[n++] = g.emac_rx_drops;
	data[n++] = g.emac_tx_frames;
	data[n++] = g.emac_tx_busy;
	data[n++] = g.dbg_log_dropped;
	data[n++] = g.loop_per_s;
	data[n++] = edev->stat_rx_frames;
	data[n++] = edev->stat_rx_bad;
	data[n++] = edev->stat_seq_gaps;
	data[n++] = edev->stat_seq_lost;
	data[n++] = edev->stat_seq_reorder;
	data[n++] = edev->stat_rx_nomem;
	data[n++] = edev->stat_rx_trunc;
	data[n++] = atomic_read(&edev->stat_tx_cn);
	data[n++] = edev->stat_cfg_retries;
	data[n++] = edev->stat_cfg_timeouts;
}

/* get_strings/get_sset_count/get_ethtool_stats signatures are identical on
 * 5.15 and 6.18. Deliberately no .get_ts_info (its struct type changed in
 * 6.11; the core falls back to software timestamps without it). */
static const struct ethtool_ops e2cf_ethtool_ops = {
	.get_strings = e2cf_get_strings,
	.get_sset_count = e2cf_get_sset_count,
	.get_ethtool_stats = e2cf_get_ethtool_stats,
};

/* ------------------------------------------------------------------ */
/* Gateway-reboot recovery                                             */
/* ------------------------------------------------------------------ */

/* Free every in-flight echo slot (their TXCs will never arrive - e.g. the
 * gateway rebooted and lost its window state) and reopen the window. */
static void e2cf_chan_flush_echo(struct e2cf_chan *chan)
{
	struct net_device *ndev = chan->ndev;
	unsigned long flags;
	unsigned int slot;

	spin_lock_irqsave(&chan->tx_lock, flags);
	for_each_set_bit(slot, &chan->echo_busy, E2CF_WIN_DEPTH) {
		can_free_echo_skb(ndev, slot, NULL);
		ndev->stats.tx_errors++;
	}
	chan->echo_busy = 0;
	spin_unlock_irqrestore(&chan->tx_lock, flags);
}

/*
 * Spec §4.8 / §7 "MCU reset": after HBs resume, the Linux side re-downloads
 * all channel configuration. A rebooted gateway lost its bittiming, channel
 * states and TXC window, so for every running channel: quiesce, drop the
 * stale echo window, re-run the ndo_open CFG sequence, and only then
 * restore carrier and wake the queue. Scheduled from softirq (e2cf_rx_hb);
 * runs in process context because CFG transactions sleep. rtnl serializes
 * the whole pass against concurrent ip link up/down.
 */
static void e2cf_reboot_work(struct work_struct *work)
{
	struct e2cf_dev *edev = container_of(work, struct e2cf_dev, reboot_work);
	bool retry = false;
	int i;

	rtnl_lock();
	for (i = 0; i < E2CF_NUM_CHANNELS; i++) {
		struct e2cf_chan *chan = edev->chan[i];
		struct net_device *ndev = chan->ndev;
		unsigned long flags;
		bool ok;

		if (!netif_running(ndev)) {
			netif_carrier_on(ndev); /* gateway is alive again */
			continue;
		}

		/* Quiesce: first suppress TXC-driven queue wakes, then stop
		 * the queue waiting out any in-flight xmit (netif_tx_disable
		 * holds the xmit lock), so the flush below cannot race a
		 * half-claimed slot or be undone by a late wake. */
		spin_lock_irqsave(&chan->tx_lock, flags);
		chan->recovering = true;
		spin_unlock_irqrestore(&chan->tx_lock, flags);
		netif_tx_disable(ndev);
		netif_carrier_off(ndev);
		e2cf_chan_flush_echo(chan);

		ok = (e2cf_chan_configure(chan) == 0);
		if (!ok) {
			/* Stay carrier-off with the queue stopped; retried
			 * via the forced alive edge below. */
			netdev_warn(ndev, "reboot recovery failed (gateway down?)\n");
			retry = true;
		}

		spin_lock_irqsave(&chan->tx_lock, flags);
		chan->recovering = false;
		spin_unlock_irqrestore(&chan->tx_lock, flags);

		if (ok) {
			chan->can.state = CAN_STATE_ERROR_ACTIVE;
			netif_carrier_on(ndev);
			netif_wake_queue(ndev);
		}
	}
	rtnl_unlock();

	if (retry) {
		/* Force an alive edge on the next HB (<= 100 ms away) so
		 * detection re-schedules this work. Covers CFG loss with no
		 * HB gap, where neither trigger would otherwise fire again. */
		edev->gw_alive = false;
		pr_warn("%s: reboot recovery incomplete - retrying on next HB\n",
			E2CF_DRV_NAME);
	} else {
		pr_info("%s: gateway reboot recovery finished\n", E2CF_DRV_NAME);
	}
}

/* ------------------------------------------------------------------ */
/* RX demux (packet_type handler, softirq)                             */
/* ------------------------------------------------------------------ */

/* Parse the records of one DATA frame (softirq): validate each record,
 * build a can/canfd skb, attach the gateway eth-egress timestamp (raw,
 * for the residency measurement) and feed it to the channel netdev. */
static void e2cf_rx_data(struct e2cf_dev *edev, const u8 *rec, u16 remain,
			 u8 count, u32 ts_base)
{
	ktime_t y = 0;

	(void)ts_base;
	/* "t4" eth-egress trailer: gateway-clock low32 ns at which the MCU
	 * handed this frame to its EQOS TX, appended after the last record
	 * (not in count). Delivered RAW (not mapped) as every record's skb
	 * hardware timestamp; the app computes MCU residency as Y - X. */
	if (remain >= E2CF_DATA_TX_TRAILER_SIZE) {
		const u8 *t = rec + remain - E2CF_DATA_TX_TRAILER_SIZE;
		u32 y_lo = (u32)t[0] | ((u32)t[1] << 8) |
			   ((u32)t[2] << 16) | ((u32)t[3] << 24);

		if (y_lo)
			y = ns_to_ktime((u64)y_lo);
	}

	while (count && remain >= E2CF_DATA_REC_HEAD_SIZE) {
		const e2cf_data_rec_t *head = (const e2cf_data_rec_t *)rec;
		u16 rec_size;
		struct e2cf_chan *chan;
		struct net_device *ndev;
		struct sk_buff *skb;

		if (head->len > 64 || (head->chan & 0x07) >= E2CF_NUM_CHANNELS) {
			edev->stat_rx_bad++;
			return;
		}
		rec_size = E2CF_DATA_REC_SIZE(head->len);
		if (rec_size > remain) {
			edev->stat_rx_bad++;
			return;
		}

		chan = edev->chan[head->chan & 0x07];
		ndev = chan->ndev;
		if (!netif_running(ndev))
			goto next;

		if (head->flags & E2CF_DATA_FLAG_FDF) {
			struct canfd_frame *cfd;

			if (!(chan->can.ctrlmode & CAN_CTRLMODE_FD)) {
				/* FD record on a classical-only channel: a
				 * conforming firmware never sends this */
				edev->stat_rx_bad++;
				goto next;
			}
			skb = alloc_canfd_skb(ndev, &cfd);
			if (!skb) {
				ndev->stats.rx_dropped++;
				goto next;
			}
			cfd->can_id = head->can_id;
			cfd->len = head->len;
			cfd->flags = 0;
			if (head->flags & E2CF_DATA_FLAG_BRS)
				cfd->flags |= CANFD_BRS;
			if (head->flags & E2CF_DATA_FLAG_ESI)
				cfd->flags |= CANFD_ESI;
			memcpy(cfd->data, &rec[E2CF_DATA_REC_HEAD_SIZE], head->len);
		} else {
			struct can_frame *cf;

			if (head->len > CAN_MAX_DLEN) {
				/* classical record with len > 8 is malformed */
				edev->stat_rx_bad++;
				goto next;
			}
			skb = alloc_can_skb(ndev, &cf);
			if (!skb) {
				ndev->stats.rx_dropped++;
				goto next;
			}
			cf->can_id = head->can_id;
			cf->len = head->len;
			if (!(head->can_id & CAN_RTR_FLAG))
				memcpy(cf->data, &rec[E2CF_DATA_REC_HEAD_SIZE], cf->len);
		}

		/* Y (eth-egress instant, gateway clock, raw) as the hardware
		 * timestamp; the kernel adds its own ingress software stamp
		 * (the app's "nicrx") for the L4 receive-path segment. */
		if (y)
			skb_hwtstamps(skb)->hwtstamp = y;

		ndev->stats.rx_packets++;
		if (!(head->can_id & CAN_RTR_FLAG))
			ndev->stats.rx_bytes += head->len;
		if (netif_rx(skb) == NET_RX_DROP)
			ndev->stats.rx_dropped++;
next:
		rec += rec_size;
		remain -= rec_size;
		count--;
	}
}

/* Parse TXC records (softirq): for each confirmed (chan, tag) slot,
 * stamp and loop back the echo skb (TXC OK) or free it (error status),
 * then wake the TX queue unless reboot recovery has quiesced it. */
static void e2cf_rx_txc(struct e2cf_dev *edev, const u8 *rec, u16 remain,
			u8 count, u32 ts_base)
{
	while (count && remain >= sizeof(e2cf_txc_rec_t)) {
		const e2cf_txc_rec_t *txc = (const e2cf_txc_rec_t *)rec;
		struct e2cf_chan *chan;
		struct net_device *ndev;
		unsigned long flags;

		if (txc->chan >= E2CF_NUM_CHANNELS || txc->tag >= E2CF_WIN_DEPTH) {
			/* skip just this record: TXC records are fixed-size, and
			 * aborting the frame would leak the echo slots of every
			 * following (valid) record - there is no slot timeout */
			edev->stat_rx_bad++;
			goto next;
		}
		chan = edev->chan[txc->chan];
		ndev = chan->ndev;

		spin_lock_irqsave(&chan->tx_lock, flags);
		if (test_and_clear_bit(txc->tag, &chan->echo_busy)) {
			if (txc->status == E2CF_TXC_OK) {
				/* Stamp the echo skb before it loops back:
				 *  - software stamp = t2 (eth-egress-from-Linux,
				 *    recorded at xmit), delivered as ts[0];
				 *  - hardware stamp = X, the request frame's
				 *    eth-ingress instant at the MCU (gateway
				 *    clock, RAW low32, NOT mapped), delivered
				 *    as ts[2]. The app pairs X with the uplink
				 *    DATA trailer Y to get MCU residency Y-X. */
				struct sk_buff *eskb =
					chan->can.echo_skb[txc->tag];

				if (eskb) {
					eskb->tstamp = chan->echo_t2[txc->tag];
					if (txc->req_eth_rx_ns)
						skb_hwtstamps(eskb)->hwtstamp =
						   ns_to_ktime((u64)txc->req_eth_rx_ns);
				}
				ndev->stats.tx_packets++;
				ndev->stats.tx_bytes +=
					can_get_echo_skb(ndev, txc->tag, NULL);
			} else {
				can_free_echo_skb(ndev, txc->tag, NULL);
				ndev->stats.tx_errors++;
				chan->stat_txc_err++;
				chan->stat_txc_rej[txc->status & 7]++;
			}
			/* Never reopen a queue that reboot recovery has
			 * quiesced - it wakes it itself after re-config. */
			if (!chan->recovering)
				netif_wake_queue(ndev);
		}
		spin_unlock_irqrestore(&chan->tx_lock, flags);
next:
		rec += sizeof(e2cf_txc_rec_t);
		remain -= sizeof(e2cf_txc_rec_t);
		count--;
	}
}

/* Parse EVT records (softirq): cache the gateway's TEC/REC/error gauges
 * for ethtool/berr readers and inject CAN state-change / error frames
 * into the stack on state transitions. */
static void e2cf_rx_evt(struct e2cf_dev *edev, const u8 *rec, u16 remain, u8 count)
{
	static const enum can_state state_map[4] = {
		CAN_STATE_ERROR_ACTIVE, CAN_STATE_ERROR_WARNING,
		CAN_STATE_ERROR_PASSIVE, CAN_STATE_BUS_OFF,
	};

	while (count && remain >= sizeof(e2cf_evt_rec_t)) {
		const e2cf_evt_rec_t *evt = (const e2cf_evt_rec_t *)rec;
		struct e2cf_chan *chan;
		enum can_state new_state;

		if (evt->chan >= E2CF_NUM_CHANNELS || evt->state > 3) {
			edev->stat_rx_bad++;
			return;
		}
		chan = edev->chan[evt->chan];
		new_state = state_map[evt->state];

		/* Cache the gateway's error gauges for ethtool/debugfs and
		 * do_get_berr_counter (previously discarded). */
		WRITE_ONCE(chan->gw_tec, evt->tec);
		WRITE_ONCE(chan->gw_rec, evt->rec);
		WRITE_ONCE(chan->gw_err_flags, evt->err_flags);
		WRITE_ONCE(chan->gw_arb_lost, evt->arb_lost_cnt);
		WRITE_ONCE(chan->gw_rx_ovf, evt->rx_ovf_cnt);

		if (netif_running(chan->ndev) && new_state != chan->can.state) {
			struct can_frame *cf;
			struct sk_buff *skb = alloc_can_err_skb(chan->ndev, &cf);

			if (new_state == CAN_STATE_BUS_OFF) {
				can_bus_off(chan->ndev);
				chan->can.state = new_state;
			} else {
				/* Attribute the transition to the dominant counter. */
				enum can_state tx_state =
					(evt->tec >= evt->rec) ? new_state : CAN_STATE_ERROR_ACTIVE;
				enum can_state rx_state =
					(evt->rec > evt->tec) ? new_state : CAN_STATE_ERROR_ACTIVE;

				can_change_state(chan->ndev, cf, tx_state, rx_state);
			}
			if (skb) {
				if (evt->err_flags & E2CF_ERR_RX_OVF) {
					cf->can_id |= CAN_ERR_CRTL;
					cf->data[1] |= CAN_ERR_CRTL_RX_OVERFLOW;
				}
				cf->data[6] = evt->tec;
				cf->data[7] = evt->rec;
				netif_rx(skb);
			}
		}

		rec += sizeof(e2cf_evt_rec_t);
		remain -= sizeof(e2cf_evt_rec_t);
		count--;
	}
}

/*
 * STATS push (spec §4.9, 1 Hz): cache the gateway's counter snapshot for
 * ethtool -S / debugfs. Forward-compatible parse: block offsets and strides
 * come from the wire header; only min(wire len, local sizeof) of each block
 * is copied, so a newer firmware appending fields keeps working.
 */
static void e2cf_rx_stats(struct e2cf_dev *edev, const u8 *body, u16 remain)
{
	const e2cf_stats_hdr_t *hdr = (const e2cf_stats_hdr_t *)body;
	unsigned long flags;
	u32 need;
	u16 glen, clen;
	u8 nchan;
	int i;

	if (remain < E2CF_STATS_HDR_SIZE ||
	    hdr->layout_rev != E2CF_STATS_LAYOUT_REV) {
		edev->stat_rx_bad++;
		return;
	}
	glen = hdr->global_len;
	clen = hdr->chan_len;
	nchan = min_t(u8, hdr->nchan, E2CF_NUM_CHANNELS);
	need = (u32)E2CF_STATS_HDR_SIZE + glen + (u32)hdr->nchan * clen;
	if ((u32)remain < need) {
		edev->stat_rx_bad++;
		return;
	}

	spin_lock_irqsave(&edev->stats_lock, flags);
	edev->gw_stats_hdr = *hdr;
	memset(&edev->gw_stats_g, 0, sizeof(edev->gw_stats_g));
	memcpy(&edev->gw_stats_g, body + E2CF_STATS_HDR_SIZE,
	       min_t(size_t, glen, sizeof(e2cf_stats_global_t)));
	for (i = 0; i < nchan; i++) {
		memset(&edev->gw_stats_ch[i], 0, sizeof(edev->gw_stats_ch[i]));
		memcpy(&edev->gw_stats_ch[i],
		       body + E2CF_STATS_HDR_SIZE + glen + (size_t)i * clen,
		       min_t(size_t, clen, sizeof(e2cf_stats_chan_t)));
	}
	edev->gw_stats_valid = true;
	edev->gw_stats_rx = jiffies;
	spin_unlock_irqrestore(&edev->stats_lock, flags);

	/* 1 Hz refresh of the per-channel gauges (EVT refreshes at 100 ms). */
	for (i = 0; i < nchan; i++) {
		WRITE_ONCE(edev->chan[i]->gw_tec, edev->gw_stats_ch[i].tec);
		WRITE_ONCE(edev->chan[i]->gw_rec, edev->gw_stats_ch[i].rec);
	}
}

/* TIME push handler (1 Hz, softirq): cache the gateway clock and its
 * CLOCK_REALTIME offset - the anchor used by e2cf_gw_to_ktime(). */
static void e2cf_rx_time(struct e2cf_dev *edev, const u8 *body, u16 remain)
{
	const e2cf_time_body_t *tb = (const e2cf_time_body_t *)body;
	unsigned long flags;

	if (remain < E2CF_TIME_BODY_SIZE)
		return;

	spin_lock_irqsave(&edev->stats_lock, flags);
	edev->gw_time_ns = tb->full_time_ns;
	edev->gw_time_flags = tb->flags;
	edev->gw_time_local_ns = ktime_get_ns();
	edev->gw_time_valid = true;
	spin_unlock_irqrestore(&edev->stats_lock, flags);
}

/* Gateway heartbeat handler (softirq): learn and lock the peer MAC,
 * track liveness, and detect gateway reboots (uptime regression or an
 * alive edge after a timeout) to schedule the recovery worker. */
static void e2cf_rx_hb(struct e2cf_dev *edev, const u8 *body, u16 remain,
		       const u8 *sa)
{
	const e2cf_hb_body_t *hb = (const e2cf_hb_body_t *)body;
	bool reboot;

	if (remain < E2CF_HB_BODY_SIZE)
		return;

	if (!edev->peer_locked) {
		memcpy(edev->peer_mac, sa, ETH_ALEN);
		edev->peer_locked = true;
		pr_info("%s: gateway locked at %pM\n", E2CF_DRV_NAME, sa);
	}
	edev->last_hb_rx = jiffies;

	/* Reboot detection (spec §7 "MCU reset": HB loss / uptime regression).
	 * uptime_s is monotonic since gateway boot, so a regression proves a
	 * reboot - including warm resets faster than the 500 ms HB timeout.
	 * An alive edge after a timeout is treated the same way: either the
	 * gateway rebooted (config + window state lost), or 500 ms of HBs
	 * were lost on the wire, in which case the idempotent reconfig is
	 * harmless. */
	reboot = edev->gw_uptime_valid && hb->uptime_s < edev->gw_uptime_s;
	edev->gw_uptime_s = hb->uptime_s;
	edev->gw_uptime_valid = true;

	if (!edev->gw_alive) {
		edev->gw_alive = true;
		if (edev->gw_ever_alive) {
			reboot = true;
		} else {
			int i;

			/* First contact since insmod: nothing to recover. */
			for (i = 0; i < E2CF_NUM_CHANNELS; i++)
				netif_carrier_on(edev->chan[i]->ndev);
		}
		edev->gw_ever_alive = true;
		pr_info("%s: gateway alive\n", E2CF_DRV_NAME);
	}

	if (reboot) {
		/* Recovery sends sleepable CFG transactions - defer out of
		 * softirq. Carrier for running channels is asserted by the
		 * work item only after their re-config succeeds (frames must
		 * not hit an unconfigured gateway channel). */
		pr_warn("%s: gateway reboot detected - recovering\n",
			E2CF_DRV_NAME);
		schedule_work(&edev->reboot_work);
	}
}

/* Frame-sequence accounting (called under rx_lock). Keeps the legacy
 * seq_gaps event counter (fires on any non-consecutive seq, including
 * mere reordering) and additionally maintains a 64-frame sliding window
 * so that losses can be told apart from reorders:
 *   - a hole pushed out of the window still unfilled = confirmed loss;
 *   - an in-window late arrival fills its hole = reorder, not a loss;
 *   - a beyond-window late arrival retro-credits the loss it was already
 *     charged as (the transport never duplicates frames, so a second
 *     arrival of the same seq cannot double-credit in practice). */
static void e2cf_seq_track(struct e2cf_dev *edev, u16 seq)
{
	s16 d;

	if (!edev->rx_seq_valid) {
		edev->rx_seq_valid = true;
		edev->rx_seq_expected = seq + 1;
		edev->rx_seq_hi = seq;
		edev->rx_seq_win = 1;
		return;
	}

	if (seq != edev->rx_seq_expected)
		edev->stat_seq_gaps++;
	edev->rx_seq_expected = seq + 1;

	d = (s16)(seq - edev->rx_seq_hi);
	if (d > 0) {
		if (d >= 64) {
			/* whole window expires: every unfilled hole in it is
			 * lost, plus the frames skipped past the new window */
			edev->stat_seq_lost += 64 - hweight64(edev->rx_seq_win)
					       + (u32)(d - 64);
			edev->rx_seq_win = 0;
		} else {
			/* the d oldest slots fall off the window's far end;
			 * unfilled ones among them are confirmed lost */
			edev->stat_seq_lost += (u32)d -
				hweight64(edev->rx_seq_win >> (64 - d));
			edev->rx_seq_win <<= d;
		}
		edev->rx_seq_win |= 1;
		edev->rx_seq_hi = seq;
	} else if (d < 0) {
		if (d >= -63) {
			u64 bit = 1ull << (u32)(-d);

			if (!(edev->rx_seq_win & bit)) {
				edev->rx_seq_win |= bit;
				edev->stat_seq_reorder++;
			}
		} else {
			edev->stat_seq_reorder++;
			if (edev->stat_seq_lost)
				edev->stat_seq_lost--;
		}
	}
	/* d == 0: duplicate - cannot happen on this transport, ignore */
}

/* packet_type RX entry for EtherType 0x88B5 (softirq): linearize and
 * validate the common header, account sequence gaps/losses, and demux
 * the frame to the per-message-type handlers above. */
static int e2cf_rcv(struct sk_buff *skb, struct net_device *dev,
		    struct packet_type *pt, struct net_device *orig_dev)
{
	struct e2cf_dev *edev = pt->af_packet_priv;
	const e2cf_hdr_t *hdr;
	const u8 *payload;
	u16 remain;
	u8 msg_type;

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb) {
		spin_lock(&edev->rx_lock);
		edev->stat_rx_nomem++;
		spin_unlock(&edev->rx_lock);
		return NET_RX_DROP;
	}
	if (!pskb_may_pull(skb, E2CF_HDR_SIZE))
		goto trunc;
	/* Linearize the whole frame: the record parsers below index the full
	 * payload, and a paged RX skb would otherwise be read out of bounds
	 * (E2CF frames are <= ~1.2 KB, so this is a bounded pull). */
	if (!pskb_may_pull(skb, skb->len))
		goto trunc;

	hdr = (const e2cf_hdr_t *)skb->data;
	if (E2CF_HDR_VER(hdr->ver_type) != E2CF_VERSION) {
		edev->stat_rx_bad++;
		goto drop;
	}

	spin_lock(&edev->rx_lock);
	edev->stat_rx_frames++;
	e2cf_seq_track(edev, hdr->seq);
	spin_unlock(&edev->rx_lock);

	msg_type = E2CF_HDR_TYPE(hdr->ver_type);
	payload = skb->data + E2CF_HDR_SIZE;
	remain = skb->len - E2CF_HDR_SIZE;

	switch (msg_type) {
	case E2CF_MSG_DATA:
		e2cf_rx_data(edev, payload, remain, hdr->count, hdr->ts_base);
		break;
	case E2CF_MSG_TXC:
		e2cf_rx_txc(edev, payload, remain, hdr->count, hdr->ts_base);
		break;
	case E2CF_MSG_EVT:
		e2cf_rx_evt(edev, payload, remain, hdr->count);
		break;
	case E2CF_MSG_CFG_RSP:
		if (remain >= E2CF_CFG_BODY_SIZE && edev->cfg_waiting) {
			const e2cf_cfg_body_t *rsp = (const e2cf_cfg_body_t *)payload;

			if (rsp->token == edev->cfg_token) {
				edev->cfg_rsp = *rsp;
				complete(&edev->cfg_done);
			}
		}
		break;
	case E2CF_MSG_HB:
		e2cf_rx_hb(edev, payload, remain, eth_hdr(skb)->h_source);
		break;
	case E2CF_MSG_TIME:
		e2cf_rx_time(edev, payload, remain);
		break;
	case E2CF_MSG_STATS:
		e2cf_rx_stats(edev, payload, remain);
		break;
	default:
		edev->stat_rx_bad++;
		break;
	}

drop:
	kfree_skb(skb);
	return NET_RX_SUCCESS;

trunc:
	spin_lock(&edev->rx_lock);
	edev->stat_rx_trunc++;
	spin_unlock(&edev->rx_lock);
	kfree_skb(skb);
	return NET_RX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Heartbeat / link supervision                                        */
/* ------------------------------------------------------------------ */

/* Reclaim echo slots whose TXC never arrived (lost ethernet frame, or the
 * gateway dropped the TXC on face starvation - the firmware explicitly
 * relies on this receiver-side timeout; spec §7). The timeout is 10x the
 * firmware's guaranteed worst-case TXC latency, so a late TXC for a
 * reclaimed slot cannot legitimately occur; if one ever arrives it is
 * ignored by the test_and_clear_bit guard in e2cf_rx_txc(). */
static void e2cf_check_txc_timeouts(struct e2cf_dev *edev)
{
	int i;

	for (i = 0; i < E2CF_NUM_CHANNELS; i++) {
		struct e2cf_chan *chan = edev->chan[i];
		struct net_device *ndev = chan->ndev;
		unsigned long flags;
		unsigned int slot;
		bool freed = false;

		if (!netif_running(ndev))
			continue;

		spin_lock_irqsave(&chan->tx_lock, flags);
		if (chan->recovering) {
			/* reboot recovery owns the window right now */
			spin_unlock_irqrestore(&chan->tx_lock, flags);
			continue;
		}
		for_each_set_bit(slot, &chan->echo_busy, E2CF_WIN_DEPTH) {
			if (!time_after(jiffies, chan->echo_ts[slot] +
					msecs_to_jiffies(E2CF_TXC_SLOT_TIMEOUT_MS)))
				continue;
			can_free_echo_skb(ndev, slot, NULL);
			clear_bit(slot, &chan->echo_busy);
			ndev->stats.tx_errors++;
			chan->stat_txc_tmo_reclaims++;
			freed = true;
		}
		spin_unlock_irqrestore(&chan->tx_lock, flags);

		if (freed) {
			if (net_ratelimit())
				netdev_warn(ndev, "TXC timeout - window slot(s) reclaimed\n");
			netif_wake_queue(ndev);
		}
	}
}

/* 100 ms heartbeat worker: transmit our HB, declare the gateway dead
 * after E2CF_HB_TIMEOUT_MS of silence (carrier off on all channels),
 * reclaim timed-out TXC slots and re-arm itself. */
static void e2cf_hb_work(struct work_struct *work)
{
	struct e2cf_dev *edev = container_of(work, struct e2cf_dev, hb_work.work);
	struct sk_buff *skb;
	u8 *body;
	e2cf_hb_body_t hb = {
		.state = E2CF_HB_STATE_READY,
		.nchan = E2CF_NUM_CHANNELS,
		.uptime_s = (u32)div_u64(ktime_get_ns(), NSEC_PER_SEC),
	};

	skb = e2cf_build_skb(edev, E2CF_MSG_HB, E2CF_HB_BODY_SIZE, 1, &body);
	if (skb) {
		memcpy(body, &hb, E2CF_HB_BODY_SIZE);
		(void)e2cf_send(edev, skb);
	}

	if (edev->gw_alive &&
	    time_after(jiffies, edev->last_hb_rx + msecs_to_jiffies(E2CF_HB_TIMEOUT_MS))) {
		int i;

		edev->gw_alive = false;
		pr_warn("%s: gateway HB timeout - carrier off\n", E2CF_DRV_NAME);
		for (i = 0; i < E2CF_NUM_CHANNELS; i++)
			netif_carrier_off(edev->chan[i]->ndev);
	}

	e2cf_check_txc_timeouts(edev);

	schedule_delayed_work(&edev->hb_work, msecs_to_jiffies(E2CF_HB_PERIOD_MS));
}

/* ------------------------------------------------------------------ */
/* debugfs: /sys/kernel/debug/eth2can/{stats,clear_stats}             */
/* ------------------------------------------------------------------ */

static const char *const e2cf_state_names[4] = {
	"ERROR-ACTIVE", "ERROR-WARNING", "ERROR-PASSIVE", "BUS-OFF",
};

/* Human-readable full dump: gateway liveness/time, the STATS push (global +
 * per channel) and the driver-side counters - the UART 's' dump, on Linux. */
static int e2cf_dbg_stats_show(struct seq_file *m, void *v)
{
	struct e2cf_dev *edev = m->private;
	e2cf_stats_hdr_t hdr;
	e2cf_stats_global_t g;
	e2cf_stats_chan_t ch[E2CF_NUM_CHANNELS];
	unsigned long flags;
	bool stats_valid, time_valid;
	unsigned long stats_rx;
	u64 time_ns, time_local_ns;
	u32 time_flags;
	int i;

	spin_lock_irqsave(&edev->stats_lock, flags);
	stats_valid = edev->gw_stats_valid;
	stats_rx = edev->gw_stats_rx;
	hdr = edev->gw_stats_hdr;
	g = edev->gw_stats_g;
	memcpy(ch, edev->gw_stats_ch, sizeof(ch));
	time_valid = edev->gw_time_valid;
	time_ns = edev->gw_time_ns;
	time_flags = edev->gw_time_flags;
	time_local_ns = edev->gw_time_local_ns;
	spin_unlock_irqrestore(&edev->stats_lock, flags);

	seq_printf(m, "gateway: %s peer=%pM hb_age_ms=%u uptime_s=%u\n",
		   edev->gw_alive ? "alive" : "DOWN",
		   edev->peer_mac,
		   jiffies_to_msecs(jiffies - edev->last_hb_rx),
		   edev->gw_uptime_valid ? edev->gw_uptime_s : 0);

	if (time_valid)
		seq_printf(m, "time: gw_ns=%llu 1588=%u local_minus_gw_ns=%lld\n",
			   time_ns, time_flags & 1,
			   (s64)(time_local_ns - time_ns));
	else
		seq_puts(m, "time: no TIME message received yet\n");

	if (!stats_valid) {
		seq_puts(m, "stats: no STATS push received (firmware without E2CF_CAP_STATS?)\n");
	} else {
		seq_printf(m, "stats: age_ms=%u layout_rev=%u gw_time_ns=%llu\n",
			   jiffies_to_msecs(jiffies - stats_rx),
			   hdr.layout_rev, hdr.gw_time_ns);
		seq_printf(m, "gw: rx=%u bad=%u untag=%u gaps=%u lost=%u reorder=%u | dn=%u rej=%u | up=%u txc=%u\n",
			   g.eth_rx_frames, g.eth_rx_bad, g.eth_rx_untagged,
			   g.seq_gaps, g.seq_lost, g.seq_reorder,
			   g.data_rx_recs, g.data_rx_rejects,
			   g.data_tx_recs, g.txc_recs);
		seq_printf(m, "gw: sent=%u sfail=%u starv=%u gated=%u cfg=%u hbrx=%u hbtx=%u logdrop=%u\n",
			   g.frames_sent, g.send_fail, g.face_starved,
			   g.rx_gated_drops, g.cfg_reqs, g.hb_rx, g.hb_tx,
			   g.dbg_log_dropped);
		seq_printf(m, "gw: emac rx=%u rxerr=%u rxdrop=%u tx=%u txbusy=%u loop=%u | peer=%u safe=%u link=%u\n",
			   g.emac_rx_frames, g.emac_rx_errors, g.emac_rx_drops,
			   g.emac_tx_frames, g.emac_tx_busy, g.loop_per_s,
			   !!(g.flags & E2CF_STATS_F_PEER_LOCKED),
			   !!(g.flags & E2CF_STATS_F_SAFE_STATE),
			   !!(g.flags & E2CF_STATS_F_ETH_LINK));
		for (i = 0; i < E2CF_NUM_CHANNELS; i++)
			seq_printf(m,
				   "gw CAN%d: rx=%u ovf=%u tx=%u rej=%u tmo=%u hwm=%u irq=%u busoff=%u %s%s tec=%u rec=%u rxovf=%u\n",
				   i, ch[i].rx_frames, ch[i].rx_ovf,
				   ch[i].tx_frames, ch[i].tx_rejected,
				   ch[i].tx_timeout, ch[i].fifo_hwm,
				   ch[i].irq_count, ch[i].bus_off_cnt,
				   e2cf_state_names[ch[i].state & 3],
				   (ch[i].flags & E2CF_STATS_CHAN_F_RUNNING) ?
					   " running" : " stopped",
				   ch[i].tec, ch[i].rec, ch[i].rx_ovf_cnt);
	}

	seq_printf(m, "drv: rx=%u bad=%u gaps=%u lost=%u reorder=%u nomem=%u trunc=%u tx_cn=%u cfg_retries=%u cfg_timeouts=%u\n",
		   edev->stat_rx_frames, edev->stat_rx_bad, edev->stat_seq_gaps,
		   edev->stat_seq_lost, edev->stat_seq_reorder,
		   edev->stat_rx_nomem, edev->stat_rx_trunc,
		   atomic_read(&edev->stat_tx_cn),
		   edev->stat_cfg_retries, edev->stat_cfg_timeouts);
	for (i = 0; i < E2CF_NUM_CHANNELS; i++) {
		struct e2cf_chan *c = edev->chan[i];

		seq_printf(m,
			   "drv %s: echo_used=%d/%u txc_tmo=%u txc_err=%u tec=%u rec=%u arb_lost=%u rx_ovf=%u\n",
			   c->ndev->name, (int)hweight_long(c->echo_busy),
			   E2CF_WIN_DEPTH, c->stat_txc_tmo_reclaims,
			   c->stat_txc_err,
			   READ_ONCE(c->gw_tec), READ_ONCE(c->gw_rec),
			   READ_ONCE(c->gw_arb_lost), READ_ONCE(c->gw_rx_ovf));
	}
	{
		u32 r_arb = 0, r_ctrl = 0, r_stop = 0, r_ovf = 0;

		for (i = 0; i < E2CF_NUM_CHANNELS; i++) {
			struct e2cf_chan *c = edev->chan[i];

			r_arb  += c->stat_txc_rej[E2CF_TXC_ARB_LOST];
			r_ctrl += c->stat_txc_rej[E2CF_TXC_CTRL_ERROR];
			r_stop += c->stat_txc_rej[E2CF_TXC_CHAN_STOPPED];
			r_ovf  += c->stat_txc_rej[E2CF_TXC_QUEUE_OVF];
		}
		/* breakdown of the non-OK TXCs: locates 'rej' by reason -
		 * chan_stopped (safe-state/not-running) vs queue_ovf (sw_txfifo
		 * full) vs ctrl_error (frame validation / stuck-TX abort) */
		seq_printf(m, "drv: txc_rej ctrl_error=%u chan_stopped=%u queue_ovf=%u arb_lost=%u\n",
			   r_ctrl, r_stop, r_ovf, r_arb);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(e2cf_dbg_stats);

/* Write "all" or a channel number 0..5: sends CFG CLEAR_STATS to the
 * gateway (process context; old firmware answers ENOTSUP -> -EINVAL).
 * A global clear also zeroes the local caches and driver counters. */
static ssize_t e2cf_dbg_clear_write(struct file *file, const char __user *ubuf,
				    size_t len, loff_t *ppos)
{
	struct e2cf_dev *edev = file->private_data;
	e2cf_cfg_body_t req = { 0 }, rsp;
	unsigned long flags;
	char buf[8];
	int ret, i;

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	strim(buf);

	req.op = E2CF_CFG_OP_CLEAR_STATS;
	if (!strcmp(buf, "all")) {
		req.chan = E2CF_CFG_CHAN_GLOBAL;
	} else if (buf[0] >= '0' && buf[0] < '0' + E2CF_NUM_CHANNELS &&
		   buf[1] == '\0') {
		req.chan = buf[0] - '0';
	} else {
		return -EINVAL;
	}

	if (!edev->gw_alive)
		return -ENETDOWN;

	ret = e2cf_cfg_xact(edev, &req, &rsp);
	if (ret)
		return ret;

	if (req.chan == E2CF_CFG_CHAN_GLOBAL) {
		spin_lock_irqsave(&edev->stats_lock, flags);
		memset(&edev->gw_stats_g, 0, sizeof(edev->gw_stats_g));
		memset(edev->gw_stats_ch, 0, sizeof(edev->gw_stats_ch));
		spin_unlock_irqrestore(&edev->stats_lock, flags);
		edev->stat_rx_frames = 0;
		edev->stat_rx_bad = 0;
		edev->stat_seq_gaps = 0;
		edev->stat_seq_lost = 0;
		edev->stat_seq_reorder = 0;
		edev->stat_rx_nomem = 0;
		edev->stat_rx_trunc = 0;
		atomic_set(&edev->stat_tx_cn, 0);
		edev->stat_cfg_retries = 0;
		edev->stat_cfg_timeouts = 0;
		for (i = 0; i < E2CF_NUM_CHANNELS; i++) {
			edev->chan[i]->stat_txc_tmo_reclaims = 0;
			edev->chan[i]->stat_txc_err = 0;
			memset(edev->chan[i]->stat_txc_rej, 0,
			       sizeof(edev->chan[i]->stat_txc_rej));
		}
	} else {
		spin_lock_irqsave(&edev->stats_lock, flags);
		memset(&edev->gw_stats_ch[req.chan], 0,
		       sizeof(edev->gw_stats_ch[req.chan]));
		spin_unlock_irqrestore(&edev->stats_lock, flags);
	}
	return len;
}

static const struct file_operations e2cf_dbg_clear_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = e2cf_dbg_clear_write,
};

/* Create /sys/kernel/debug/eth2can/{stats,clear_stats}. */
static void e2cf_debugfs_init(struct e2cf_dev *edev)
{
	edev->dbg_dir = debugfs_create_dir(E2CF_DRV_NAME, NULL);
	debugfs_create_file("stats", 0444, edev->dbg_dir, edev,
			    &e2cf_dbg_stats_fops);
	debugfs_create_file("clear_stats", 0200, edev->dbg_dir, edev,
			    &e2cf_dbg_clear_fops);
}

/* ------------------------------------------------------------------ */
/* Setup / teardown                                                    */
/* ------------------------------------------------------------------ */

static int e2cf_add_channel(struct e2cf_dev *edev, u8 idx)
{
	struct net_device *ndev;
	struct e2cf_chan *chan;
	int err;

	ndev = alloc_candev(sizeof(*chan), E2CF_WIN_DEPTH);
	if (!ndev)
		return -ENOMEM;

	chan = netdev_priv(ndev);
	chan->edev = edev;
	chan->ndev = ndev;
	chan->ch = idx;
	spin_lock_init(&chan->tx_lock);

	ndev->netdev_ops = &e2cf_netdev_ops;
	ndev->ethtool_ops = &e2cf_ethtool_ops;
	ndev->flags |= IFF_ECHO;
	snprintf(ndev->name, IFNAMSIZ, "eth2can%u", idx);

	chan->can.clock.freq = E2CF_CAN_CLK_HZ;
	chan->can.bittiming_const = &e2cf_nom_bittiming_const;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	chan->can.fd.data_bittiming_const = &e2cf_dat_bittiming_const;
#else
	chan->can.data_bittiming_const = &e2cf_dat_bittiming_const;
#endif
	/* No FD_NON_ISO: v1 firmware always runs ISO FD (ISOCANFDEN set) and
	 * rejects E2CF_MODE_NONISO with ENOTSUP. Re-add once implemented. */
	chan->can.ctrlmode_supported = CAN_CTRLMODE_FD | CAN_CTRLMODE_LISTENONLY |
				       CAN_CTRLMODE_LOOPBACK;
	chan->can.do_get_berr_counter = e2cf_get_berr_counter;
	chan->can.state = CAN_STATE_STOPPED;

	err = register_candev(ndev);
	if (err) {
		free_candev(ndev);
		return err;
	}

	netif_carrier_off(ndev); /* on at first gateway HB */
	edev->chan[idx] = chan;
	return 0;
}

/* Module init: resolve the lower netdev, register the six candevs, hook
 * the EtherType intercept, program the HB discovery multicast into the
 * NIC filter and start the heartbeat worker. */
static int __init e2cf_init(void)
{
	struct e2cf_dev *edev;
	int i, err;

	edev = kzalloc(sizeof(*edev), GFP_KERNEL);
	if (!edev)
		return -ENOMEM;

	edev->lower = dev_get_by_name(&init_net, ifname);
	if (!edev->lower) {
		pr_err("%s: no such interface: %s\n", E2CF_DRV_NAME, ifname);
		kfree(edev);
		return -ENODEV;
	}

	spin_lock_init(&edev->seq_lock);
	spin_lock_init(&edev->stats_lock);
	spin_lock_init(&edev->rx_lock);
	mutex_init(&edev->cfg_lock);
	init_completion(&edev->cfg_done);
	INIT_DELAYED_WORK(&edev->hb_work, e2cf_hb_work);
	INIT_WORK(&edev->reboot_work, e2cf_reboot_work);

	if (peer && strlen(peer)) {
		u8 mac[ETH_ALEN];

		if (!mac_pton(peer, mac)) {
			pr_err("%s: bad peer MAC '%s'\n", E2CF_DRV_NAME, peer);
			err = -EINVAL;
			goto err_put;
		}
		memcpy(edev->peer_mac, mac, ETH_ALEN);
		edev->peer_locked = true;
	}

	for (i = 0; i < E2CF_NUM_CHANNELS; i++) {
		err = e2cf_add_channel(edev, i);
		if (err)
			goto err_chans;
	}

	/* The intercept: ONLY EtherType 0x88B5 on this one interface is
	 * steered here; everything else continues into the regular stack. */
	edev->pt.type = htons(E2CF_ETHERTYPE);
	edev->pt.dev = edev->lower;
	edev->pt.func = e2cf_rcv;
	edev->pt.af_packet_priv = edev;
	dev_add_pack(&edev->pt);

	/* Program the HB discovery multicast group into the lower NIC's
	 * hardware filter (the firmware does the same via
	 * ENET_QOS_AddMulticastGroup). Without this, the gateway's
	 * pre-peer-lock multicast HBs are dropped by the MAC filter and
	 * discovery deadlocks - masked whenever tcpdump (promiscuous mode)
	 * happens to be running. */
	rtnl_lock();
	err = dev_mc_add(edev->lower, e2cf_hb_mcast);
	rtnl_unlock();
	if (err)
		pr_warn("%s: failed to add HB multicast filter (%d); discovery may require promiscuous mode\n",
			E2CF_DRV_NAME, err);

	edev->last_hb_rx = jiffies;
	schedule_delayed_work(&edev->hb_work, msecs_to_jiffies(E2CF_HB_PERIOD_MS));

	e2cf_debugfs_init(edev);
	g_edev = edev;
	pr_info("%s: phase-1 transport on %s (vid=%d), 6 channels registered\n",
		E2CF_DRV_NAME, ifname, vid);
	return 0;

err_chans:
	while (--i >= 0) {
		unregister_candev(edev->chan[i]->ndev);
		free_candev(edev->chan[i]->ndev);
	}
err_put:
	dev_put(edev->lower);
	kfree(edev);
	return err;
}

/* Module exit: tear down in dependency order - debugfs, heartbeat
 * worker, multicast filter entry, packet hook (quiesces the softirq
 * path), reboot worker, candevs, lower netdev reference. */
static void __exit e2cf_exit(void)
{
	struct e2cf_dev *edev = g_edev;
	int i;

	debugfs_remove_recursive(edev->dbg_dir);
	cancel_delayed_work_sync(&edev->hb_work);
	rtnl_lock();
	dev_mc_del(edev->lower, e2cf_hb_mcast);
	rtnl_unlock();
	dev_remove_pack(&edev->pt); /* synchronizes: no new softirq schedules */
	cancel_work_sync(&edev->reboot_work);
	for (i = 0; i < E2CF_NUM_CHANNELS; i++) {
		unregister_candev(edev->chan[i]->ndev);
		free_candev(edev->chan[i]->ndev);
	}
	dev_put(edev->lower);
	kfree(edev);
}

module_init(e2cf_init);
module_exit(e2cf_exit);

MODULE_AUTHOR("Ken Li <ken.li@nxp.com>");
MODULE_DESCRIPTION("E2CF ethernet-to-CANFD gateway, 6x SocketCAN (phase-1 transport)");
MODULE_LICENSE("GPL");
