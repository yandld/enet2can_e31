/*
 * can_service.h - CAN service boundary for the CAN0-first gateway slice
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef CAN_SERVICE_H_
#define CAN_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "can_gateway_protocol.h"

#define CAN_SERVICE_CHANNEL_COUNT 6U
#define CAN_ACTIVE_MASK 0x01U
#define CAN_SERVICE_RX_RING_SIZE 32U
#define CAN_SERVICE_TX_QUEUE_SIZE 32U
#define CAN_SERVICE_USE_ENHANCED_RX_FIFO 1U
#define CAN_SERVICE_EFIFO_BATCH_SIZE 1U
#define CAN_SERVICE_EFIFO_WATERMARK 0U

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
    can_service_state_t state;
    uint32_t bitRate;
    uint32_t bitRateFD;
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
can_service_status_t can_service_get_status(uint8_t channel);
can_service_config_t can_service_get_config(uint8_t channel);
uint32_t can_service_set_config(uint8_t channel, const can_service_config_t *config);
uint32_t can_service_get_last_config_status(void);
void can_service_tick_1ms(void);

#endif /* CAN_SERVICE_H_ */
