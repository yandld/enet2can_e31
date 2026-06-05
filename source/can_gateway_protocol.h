/*
 * can_gateway_protocol.h - SocketCAN-first UDP tunnel wire format
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Wire format v3: variable-length frame records.
 *   packet = header (16 bytes) + N frame records
 *   record = 16-byte fixed head {channel,flags,dlc,reserved,can_id,timestamp,status}
 *            followed by dlc_to_len(dlc) payload bytes (0..64).
 * v2 sent a fixed 64-byte payload per frame regardless of dlc; v3 sends only the
 * bytes the frame actually carries, which cuts wire/CPU cost up to ~5x for small
 * frames and lets one MTU-sized packet hold many more frames.
 */
#ifndef CAN_GATEWAY_PROTOCOL_H_
#define CAN_GATEWAY_PROTOCOL_H_

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

#define CAN_GATEWAY_STATUS_OK 0x00000000UL
#define CAN_GATEWAY_STATUS_CAN_TX_BUSY 0x00000001UL
#define CAN_GATEWAY_STATUS_CAN_TX_ERROR 0x00000002UL
#define CAN_GATEWAY_STATUS_CAN_RX_OVERFLOW 0x00000004UL
#define CAN_GATEWAY_STATUS_INVALID_PACKET 0x00000008UL
#define CAN_GATEWAY_STATUS_DISABLED_CHANNEL 0x00000010UL
#define CAN_GATEWAY_STATUS_QUEUE_FULL 0x00000020UL
#define CAN_GATEWAY_STATUS_NO_SESSION 0x00000040UL
#define CAN_GATEWAY_STATUS_PARSE_ERROR 0x00000080UL

/* Fixed bytes of a frame record on the wire (everything except the payload). */
#define CAN_GATEWAY_FRAME_HEAD_SIZE 16U
/* Largest possible frame record: head + 64-byte FD payload. */
#define CAN_GATEWAY_MAX_FRAME_RECORD (CAN_GATEWAY_FRAME_HEAD_SIZE + CAN_GATEWAY_MAX_DATA_LEN)

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

/*
 * In-memory frame representation. The first CAN_GATEWAY_FRAME_HEAD_SIZE bytes
 * (channel..status) are copied verbatim to/from the wire; only dlc_to_len(dlc)
 * bytes of data follow on the wire.
 */
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
    /* Board-internal DWT ingress timestamp (cycles). Carried with the frame
     * through the service queues for latency measurement only; NOT serialized
     * (the wire copies just the 16-byte head + payload, never the whole struct). */
    uint32_t ingress_cycles;
} can_gateway_frame_t;

typedef struct
{
    can_gateway_packet_header_t header;
    can_gateway_frame_t frames[CAN_GATEWAY_MAX_FRAMES_PER_PACKET];
} can_gateway_packet_t;
#pragma pack(pop)

#define CAN_GATEWAY_PACKET_HEADER_SIZE 16U
/* Worst-case datagram: header + all frames at max record size. Stays under a 1500-byte MTU. */
#define CAN_GATEWAY_MAX_PACKET_BYTES \
    (CAN_GATEWAY_PACKET_HEADER_SIZE + (CAN_GATEWAY_MAX_FRAMES_PER_PACKET * CAN_GATEWAY_MAX_FRAME_RECORD))

#endif /* CAN_GATEWAY_PROTOCOL_H_ */
