/*
 * can_gateway_protocol.h - SocketCAN-first UDP tunnel wire format
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef CAN_GATEWAY_PROTOCOL_H_
#define CAN_GATEWAY_PROTOCOL_H_

#include <stdint.h>

#define CAN_GATEWAY_MAGIC 0x53434757UL /* "SCGW" */
#define CAN_GATEWAY_VERSION 2U
#define CAN_GATEWAY_MAX_CHANNELS 6U
#define CAN_GATEWAY_MAX_DATA_LEN 64U
#define CAN_GATEWAY_MAX_FRAMES_PER_PACKET 8U

#define CAN_GATEWAY_UDP_DATA_PORT 50000U
#define CAN_GATEWAY_UDP_CONTROL_PORT 50001U

#define CAN_GATEWAY_PACKET_TYPE_FRAMES 1U

#define CAN_GATEWAY_FLAG_FD 0x01U
#define CAN_GATEWAY_FLAG_BRS 0x02U
#define CAN_GATEWAY_FLAG_EXTENDED_ID 0x04U
#define CAN_GATEWAY_FLAG_REMOTE 0x08U
#define CAN_GATEWAY_FLAG_ERROR 0x10U

#define CAN_GATEWAY_STATUS_OK 0x00000000UL
#define CAN_GATEWAY_STATUS_CAN_TX_BUSY 0x00000001UL
#define CAN_GATEWAY_STATUS_CAN_TX_ERROR 0x00000002UL
#define CAN_GATEWAY_STATUS_CAN_RX_OVERFLOW 0x00000004UL
#define CAN_GATEWAY_STATUS_INVALID_PACKET 0x00000008UL
#define CAN_GATEWAY_STATUS_DISABLED_CHANNEL 0x00000010UL
#define CAN_GATEWAY_STATUS_QUEUE_FULL 0x00000020UL
#define CAN_GATEWAY_STATUS_NO_SESSION 0x00000040UL
#define CAN_GATEWAY_STATUS_PARSE_ERROR 0x00000080UL

#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint8_t version;
    uint8_t packet_type;
    uint16_t frame_count;
    uint32_t sequence;
    uint32_t status;
} can_gateway_packet_header_t;

typedef struct
{
    uint8_t channel;
    uint8_t flags;
    uint8_t dlc;
    uint8_t reserved;
    uint32_t can_id;
    uint32_t timestamp;
    uint32_t status;
    uint8_t data[64];
} can_gateway_frame_t;

typedef struct
{
    can_gateway_packet_header_t header;
    can_gateway_frame_t frames[CAN_GATEWAY_MAX_FRAMES_PER_PACKET];
} can_gateway_packet_t;
#pragma pack(pop)

#endif /* CAN_GATEWAY_PROTOCOL_H_ */
