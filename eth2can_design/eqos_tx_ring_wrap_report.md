# EQOS TX descriptor-ring wrap anomaly on MCXE31B (frame skipped + stale frame retransmitted)

Status: reproduced and wire-captured; root cause localized to the TX
descriptor-ring wrap edge below the application code; project-local
workaround applied (see "Workarounds"). Filed for MCUXpresso SDK /
EQOS IP review.

Contact: Ken Li <ken.li@nxp.com>

## Environment

- MCXE31B (Cortex-M7 @ 160 MHz), EMAC = Synopsys EQOS (`ENET_QOS`),
  RMII 100M full duplex, LAN8741 PHY
- MCUXpresso SDK `fsl_enet_qos.c` (drivers/ copy in this repo)
- TX descriptor ring: 8 descriptors, single channel, zero-copy
  single-buffer frames from non-cacheable DTCM
  (`AT_NONCACHEABLE_SECTION_ALIGN`), descriptors also in non-cacheable
  DTCM
- All `ENET_QOS_SendFrame()` calls fully IRQ-masked (PRIMASK);
  `ENET_QOS_ReclaimTxDescriptor()` runs from the EMAC IRQ handler
- Traffic: ~6,000 frames/s steady (60-170 B frames), egress mostly
  idle between frames, so the TX DMA suspends and is doorbell-resumed
  for nearly every frame

## Observation

The application stamps every TX frame with an incrementing 16-bit
sequence number, and submissions are never rejected
(`kStatus_ENET_QOS_TxFrameBusy` count = 0), so on-wire
`seq mod 8` equals the TX descriptor index used for that frame.

Receiver-side capture (tcpdump on the peer, 203,240 frames, no capture
drops) caught **two** anomaly events, both with the identical
fingerprint:

```
event 1                                   event 2
[39793] seq=2205 len=170 ts_base=780826962   seq=56877 (original)
[39794] seq=2206
[39795] seq=2207
[39796] seq=2205 len=170 ts_base=780826962   seq=56877 (byte-identical COPY)
[39797] seq=2209  <- seq 2208 NEVER appears  seq=56880 never appears
```

- The duplicate is **byte-identical** to the original (same seq, same
  ts_base payload, same length) - it was not rebuilt by software (the
  application rewrites the sequence number on every send), the same
  buffer bytes were transmitted twice by the DMA.
- The lost frame had been submitted normally (`SendFrame` returned
  `kStatus_Success`, accounting consistent) but never reached the wire.

**Descriptor-slot fingerprint** (the decisive evidence):

| event | lost frame | lost mod 8 | duplicated frame | dup mod 8 |
|---|---|---|---|---|
| 1 | seq 2208  | **0** (wrap slot) | seq 2205  | 5 |
| 2 | seq 56880 | **0** (wrap slot) | seq 56877 | 5 |

Both lost frames sat in descriptor slot 0 - the ring-wrap slot - and
both retransmissions came from slot 5. Random loss matching this
pattern twice has probability ~1e-4; this is a deterministic wrap-edge
mechanism.

Rate: 2 events per ~200k frames (~1e-5) under continuous
suspend/resume operation.

## Ruled out

- Application double-send: impossible to produce identical sequence
  numbers (seq is assigned inside the send path, masked).
- Descriptor write ordering: `ENET_QOS_ConfigTxDescriptor()` correctly
  writes buffer fields, `__DMB()`, OWN last, `__DSB()`.
- Reclaim/submit preemption: `txDescUsed` updates are IRQ-masked in
  both paths; submit path fully masked; index walk verified consistent.
- Buffer staleness: descriptors and frame buffers live in non-cacheable
  DTCM; single core; the pre-OWN `__DMB()` orders the payload stores.
- Receiver side: the duplicate+gap is present on the wire itself
  (capture taken on a direct cable, no switch).

## Suspected mechanism

`ENET_QOS_SendFrame()` computes the tail pointer as one descriptor
past the ring end when `txGenIdx` wraps to 0:

```c
    txDesc = &txBdRing->txBdBase[txBdRing->txGenIdx];
    if (txBdRing->txGenIdx == 0U)
    {
        txDesc = &txBdRing->txBdBase[txBdRing->txRingLen];  /* one PAST the end */
    }
```

Combined with the DMA being suspended (all OWN bits consumed) and
doorbell-resumed at exactly this edge, the DMA appears to mis-resolve
the out-of-ring tail address: it skips the freshly queued slot-0
descriptor and re-processes an older descriptor image (slot 5 in both
captures). Linux stmmac drives the same Synopsys IP with the tail
pointer always kept inside the ring (it simply wraps to the base) and
instead never fills the ring completely.

## Workarounds applied in this project

1. `E2CF_ETH_TXBD_NUM` 8 -> 64 (wrap rate /8; any residual event moving
   to a mod-64 fingerprint independently confirms the diagnosis).
2. `fsl_enet_qos.c` `ENET_QOS_SendFrame()`: tail pointer kept inside
   the ring (wraps to base, stmmac-style), marked `E2CF WORKAROUND`.
3. `eth_raw_send()` keeps one descriptor slot permanently unused so the
   in-ring tail can never alias the oldest pending descriptor.

## Reproduction

Bidirectional 0x88B5 traffic at >= 5k frames/s for >= 30 s with
egress-idle (suspend/resume per frame) submission; capture on the link
peer and scan for duplicate/missing 16-bit sequence numbers; correlate
`seq mod ring_len`. Tooling in this repo: `linux/can_testcase/canperf`
(`--gap-us 1`) plus the eth2can driver's `drvg_seq_lost` /
`drvg_seq_gaps` counters.
