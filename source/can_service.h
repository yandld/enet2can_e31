/*
 * can_service.h - CAN service boundary for the six-channel gateway
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef CAN_SERVICE_H_
#define CAN_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "can_gateway_protocol.h"
#include "latency_timer.h"

#define CAN_SERVICE_CHANNEL_COUNT 6U
#define CAN_ACTIVE_MASK 0x3FU
#define CAN_SERVICE_RX_RING_SIZE 64U
#define CAN_SERVICE_TX_QUEUE_SIZE 64U
/* RX slots reserved so error events are never starved by data frames. */
#define CAN_SERVICE_RX_ERROR_HEADROOM 4U
/* Max frames drained from one channel's RX mailbox bank per poll pass. */
#define CAN_SERVICE_RX_DRAIN_MAX 48U
/* Max TX message buffers per channel (all channels use 2). */
#define CAN_SERVICE_MAX_TX_MB 2U
/* Per-channel FIFO of pending UDP-in stamps for loopback end-to-end latency.
 * Bounds in-flight frames (TX MBs + RX MB bank); 16 leaves ample margin. */
#define CAN_SERVICE_E2E_RING_SIZE 16U

#define CAN_BITRATE 1000000U
#define CAN_USE_CANFD 1
#define CAN_FD_BITRATE 5000000U
#define CAN_CLASSIC_DLC 8U
#define CAN_FD_DLC 15U

typedef enum
{
    CAN_SERVICE_STATE_ERROR_ACTIVE = 0,
    CAN_SERVICE_STATE_WARNING,
    CAN_SERVICE_STATE_ERROR_PASSIVE,
    CAN_SERVICE_STATE_BUS_OFF
} can_service_state_t;

typedef enum
{
    CAN_SERVICE_FILTER_ACCEPT_ALL = 0,
    CAN_SERVICE_FILTER_ID_MASK
} can_service_filter_mode_t;

typedef enum
{
    CAN_SERVICE_TX_DROP_NEWEST = 0,
    CAN_SERVICE_TX_DROP_OLDEST
} can_service_tx_drop_policy_t;

typedef enum
{
    CAN_SERVICE_CONFIG_OK = 0,
    CAN_SERVICE_CONFIG_UNSUPPORTED_CHANNEL,
    CAN_SERVICE_CONFIG_NULL,
    CAN_SERVICE_CONFIG_INVALID_BITRATE,
    CAN_SERVICE_CONFIG_INVALID_MODE,
    CAN_SERVICE_CONFIG_INVALID_FILTER
} can_service_config_status_t;

typedef struct
{
    bool enabled;
    bool useFD;
    bool brs;
    bool loopback; /* internal self-test loopback: TX is looped back to RX on-chip,
                    * self-ACKed, no bus/transceiver needed (for the no-wiring tests) */
    uint32_t bitRate;
    uint32_t bitRateFD;
    can_service_filter_mode_t filterMode;
    uint32_t filterId;
    uint32_t filterMask;
    can_service_tx_drop_policy_t txDropPolicy;
} can_service_config_t;

typedef struct
{
    bool enabled;
    bool useFD;
    bool enhancedRxFifo;
    can_service_state_t state;
    uint32_t bitRate;
    uint32_t bitRateFD;
    uint32_t rxCapacity;
    uint32_t txCapacity;
    uint32_t rxDrainMax;
    uint32_t rxHwSlots;
    uint32_t txMbCount;
    uint32_t rxMbCount;
    uint32_t rxCount;
    uint32_t txStartCount;
    uint32_t txDoneCount;
    uint32_t txBusyCount;
    uint32_t txErrorCount;
    uint32_t txTimeoutCount;
    uint32_t txDropCount;
    uint32_t rxDropCount;
    uint32_t rxErrorCount;
    uint32_t rxQueued;
    uint32_t txQueued;
    uint32_t rxQueueWatermark;
    uint32_t txQueueWatermark;
    uint32_t rxFifoIdleCount;
    uint32_t rxFifoOverflowCount;
    uint32_t rxFifoWarningCount;
    uint32_t rxFifoUnderflowCount;
    uint32_t busOffCount;
    uint32_t txErrCounter;
    uint32_t rxErrCounter;
    uint32_t lastErrorStatus;
} can_service_status_t;

bool can_service_init(uint32_t activeMask);
void can_service_poll(void);
uint32_t can_service_send(uint8_t channel, const can_gateway_frame_t *frame);
bool can_service_read(uint8_t channel, can_gateway_frame_t *frame);
uint16_t can_service_rx_available(uint8_t channel);
bool can_service_peek(uint8_t channel, uint16_t offset, can_gateway_frame_t *frame);
void can_service_consume(uint8_t channel, uint16_t count);
can_service_status_t can_service_get_status(uint8_t channel);
can_service_config_t can_service_get_config(uint8_t channel);
/* Board-internal UDP-in -> CAN-MB-write latency (cycles), for the status report. */
latency_stat_t can_service_get_udp_to_can_latency(void);
uint32_t can_service_set_config(uint8_t channel, const can_service_config_t *config);
uint32_t can_service_get_last_config_status(void);
void can_service_reset_stats(void);
void can_service_tick_1ms(void);

#endif /* CAN_SERVICE_H_ */
