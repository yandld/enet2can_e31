/*
 * can_gateway_wire.h - MCXE31B SocketCAN gateway "SCGW v3" wire format (shared).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Wire-only view of the firmware header source/can_gateway_protocol.h: the 16-byte
 * packet header and the 16-byte frame-record head, plus the DLC<->length helpers.
 * The firmware's in-memory frame struct also carries data[64] and an internal
 * ingress_cycles field; NEITHER is part of the wire, so they live only in the
 * firmware header. Both ends MUST keep these constants and the 16-byte head field
 * offsets identical - `make check-proto` diffs the #define constants and static-asserts
 * the head field offsets against source/can_gateway_protocol.h.
 *
 * Wire layout (little-endian):
 *   packet = header(16) + N frame records
 *   record = head(16){channel,flags,dlc,reserved,can_id,timestamp,status}
 *            + can_gateway_wire_data_len(flags,dlc) payload bytes (0..64)
 */
#ifndef CAN_GATEWAY_WIRE_H_
#define CAN_GATEWAY_WIRE_H_

#include <stdint.h>

#define CAN_GATEWAY_MAGIC 0x53434757UL /* "SCGW" */
#define CAN_GATEWAY_VERSION 3U
#define CAN_GATEWAY_MAX_CHANNELS 6U
#define CAN_GATEWAY_MAX_DATA_LEN 64U
#define CAN_GATEWAY_MAX_FRAMES_PER_PACKET 16U

#define CAN_GATEWAY_UDP_DATA_PORT 50000U
#define CAN_GATEWAY_UDP_CONTROL_PORT 50001U

#define CAN_GATEWAY_PACKET_TYPE_FRAMES 1U

#define CAN_GATEWAY_FLAG_FD 0x01U
#define CAN_GATEWAY_FLAG_BRS 0x02U
#define CAN_GATEWAY_FLAG_EXTENDED_ID 0x04U
#define CAN_GATEWAY_FLAG_REMOTE 0x08U
#define CAN_GATEWAY_FLAG_ERROR 0x10U

#define CAN_GATEWAY_PACKET_HEADER_SIZE 16U
#define CAN_GATEWAY_FRAME_HEAD_SIZE 16U
#define CAN_GATEWAY_MAX_FRAME_RECORD (CAN_GATEWAY_FRAME_HEAD_SIZE + CAN_GATEWAY_MAX_DATA_LEN)
#define CAN_GATEWAY_MAX_PACKET_BYTES \
    (CAN_GATEWAY_PACKET_HEADER_SIZE + (CAN_GATEWAY_MAX_FRAMES_PER_PACKET * CAN_GATEWAY_MAX_FRAME_RECORD))

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

/* Wire head of one frame record; followed by the variable-length payload. */
typedef struct
{
    uint8_t channel;
    uint8_t flags;
    uint8_t dlc;
    uint8_t reserved;
    uint32_t can_id;
    uint32_t timestamp;
    uint32_t status;
} can_gateway_frame_head_t;
#pragma pack(pop)

/* CAN FD DLC code -> payload length. */
static inline uint8_t can_gateway_dlc_to_len(uint8_t dlc)
{
    static const uint8_t table[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return (dlc < 16U) ? table[dlc] : 0U;
}

/* CAN FD payload length -> smallest DLC code that fits. */
static inline uint8_t can_gateway_len_to_dlc(uint8_t len)
{
    if (len <= 8U) return len;
    if (len <= 12U) return 9U;
    if (len <= 16U) return 10U;
    if (len <= 20U) return 11U;
    if (len <= 24U) return 12U;
    if (len <= 32U) return 13U;
    if (len <= 48U) return 14U;
    return 15U;
}

/* Payload bytes carried on the wire for one frame (must match the firmware). */
static inline uint8_t can_gateway_wire_data_len(uint8_t flags, uint8_t dlc)
{
    if (flags & CAN_GATEWAY_FLAG_FD) return can_gateway_dlc_to_len(dlc);
    return (dlc < 8U) ? dlc : 8U;
}

#endif /* CAN_GATEWAY_WIRE_H_ */
