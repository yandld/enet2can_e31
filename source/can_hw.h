/*
 * can_hw.h - six-channel FlexCAN layer of the E2CF gateway
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Resource plan (design doc 03 §3.3, hardware asymmetry confirmed 2026-06-11):
 *   CAN0   : Enhanced RX FIFO (20 deep, the only instance that has one),
 *            drained from interrupt (eDMA offload = Phase 2), 1 TX MB.
 *   CAN1/2 : 8 RX MBs (IRMQ reception queue) + 1 TX MB, MB interrupts.
 *   CAN3-5 : 4 RX MBs + 1 TX MB, MB interrupts.
 *   TX     : single ACTIVE TX MB per channel - strict per-channel ordering
 *            (multiple TX MBs arbitrate by ID priority and would reorder).
 *   RX MB banks are drained one full scan per IRQ and sorted by the 16-bit
 *   free-running-timer capture before delivery (arrival order != MB index
 *   order); the SDK MB read implements the lock/unlock protocol.
 *
 * Context model: RX/TXC callbacks run at CAN IRQ priority (1); submit_tx runs
 * at EMAC IRQ priority (2); config/poll functions run in the main loop.
 */
#ifndef CAN_HW_H_
#define CAN_HW_H_

#include <stdbool.h>
#include <stdint.h>

#include "e2cf_config.h"
#include "e2cf_proto.h"

/* Raw bit timing exactly as carried by CFG SET_BITRATE (spec §4.6). */
typedef struct
{
    uint16_t nom_brp;
    uint8_t nom_tseg1; /* PROP_SEG + PHASE_SEG1, tq */
    uint8_t nom_tseg2;
    uint8_t nom_sjw;
    uint8_t dat_brp;
    uint8_t dat_tseg1;
    uint8_t dat_tseg2;
    uint8_t dat_sjw;
    uint8_t tdc_off;
    uint16_t mode_flags; /* E2CF_MODE_* */
} can_hw_bittiming_t;

typedef struct
{
    uint32_t rx_frames;
    uint32_t rx_ovf;       /* eFIFO overflow / MB overrun losses */
    uint32_t tx_frames;    /* completed on the bus */
    uint32_t tx_rejected;  /* submit-time rejects (stopped/overflow/invalid) */
    uint32_t tx_timeout;   /* aborted stuck transmissions */
    uint32_t fifo_hwm;     /* sw_txfifo high-water mark */
    uint32_t irq_count;
    uint32_t bus_off_cnt;
    uint16_t arb_lost_cnt;
} can_hw_stats_t;

bool can_hw_init(void);

/* CFG operations; return an E2CF_CFG_* status. Main-loop / EMAC-ISR context. */
uint8_t can_hw_set_bitrate(uint8_t ch, const can_hw_bittiming_t *bt);
uint8_t can_hw_set_filter(uint8_t ch, uint8_t bank, uint32_t can_id, uint32_t can_mask);
uint8_t can_hw_start(uint8_t ch);
uint8_t can_hw_stop(uint8_t ch);
uint8_t can_hw_reset(uint8_t ch);
/* Zero this channel's counters without touching the controller or its
 * running state (the non-destructive half of can_hw_reset). */
uint8_t can_hw_clear_stats(uint8_t ch);
bool can_hw_is_running(uint8_t ch);
void can_hw_get_bitrate(uint8_t ch, can_hw_bittiming_t *out);

/*
 * Queue one Linux->MCU DATA record for transmission. `head` is the 8-byte
 * record head, `payload` points at the following `head->len` bytes.
 * `eth_rx_ns` is the gateway-clock low32 ns at which the request frame
 * carrying this record arrived at the EMAC - threaded through to the TXC
 * (req_eth_rx_ns) so the host can measure MCU residency.
 * Returns E2CF_TXC_OK when queued (TXC follows on bus completion) or a
 * terminal E2CF_TXC_* status when rejected (caller must emit the TXC NOW).
 */
uint8_t can_hw_submit_tx(const e2cf_data_rec_t *head, const uint8_t *payload,
                         uint32_t eth_rx_ns);

/* Safe-state gate (HB supervision): true = stop loading new TX MBs. */
void can_hw_set_tx_gate(bool gated);

/* Poll error counters / fault state; call every ~10 ms from the main loop.
 * Emits e2cf_core_can_state_change() on worsening transitions. */
void can_hw_poll_errors(void);

/* Snapshot the current EVT record for periodic 100 ms reporting. */
void can_hw_get_evt(uint8_t ch, e2cf_evt_rec_t *evt);

const can_hw_stats_t *can_hw_stats(uint8_t ch);
void can_hw_dump_stats(void);

/* ---- upcalls implemented by e2cf_core (called from CAN IRQ, prio 1) ---- */
void e2cf_core_can_rx(uint8_t ch, uint32_t can_id, uint8_t len, uint8_t flags,
                      const uint8_t *data, uint32_t rx_ns_lo32);
void e2cf_core_can_txc(uint8_t ch, uint8_t tag, uint8_t status, uint32_t ns_lo32,
                       uint32_t req_eth_rx_ns);
void e2cf_core_can_state_change(uint8_t ch, const e2cf_evt_rec_t *evt);

#endif /* CAN_HW_H_ */
