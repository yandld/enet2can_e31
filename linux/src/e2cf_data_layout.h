/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef E2CF_DATA_LAYOUT_H_
#define E2CF_DATA_LAYOUT_H_

#include "e2cf_proto.h"

static inline int e2cf_data_trailer_offset(const uint8_t *records,
					   uint16_t payload_len,
					   uint8_t count,
					   uint16_t *trailer_offset)
{
	uint16_t offset = 0;
	uint8_t index;

	if (!records || !trailer_offset)
		return 0;

	for (index = 0; index < count; index++) {
		const e2cf_data_rec_t *head;
		uint16_t record_size;

		if ((uint16_t)(payload_len - offset) < E2CF_DATA_REC_HEAD_SIZE)
			return 0;

		head = (const e2cf_data_rec_t *)(records + offset);
		record_size = (uint16_t)E2CF_DATA_REC_SIZE(head->len);
		if (record_size > (uint16_t)(payload_len - offset))
			return 0;

		offset = (uint16_t)(offset + record_size);
	}

	if ((uint16_t)(payload_len - offset) < E2CF_DATA_TX_TRAILER_SIZE)
		return 0;

	*trailer_offset = offset;
	return 1;
}

#endif /* E2CF_DATA_LAYOUT_H_ */
