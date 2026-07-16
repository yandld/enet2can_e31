#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP_DIR=${TMPDIR:-/tmp}/e2cf-data-layout-$$
CC=${CC:-cc}

cleanup()
{
	rm -rf -- "$TMP_DIR"
}
trap cleanup EXIT HUP INT TERM
mkdir -p -- "$TMP_DIR"

"$CC" -std=c11 -Wall -Wextra -Werror \
	-I"$ROOT_DIR/linux/src" -x c -o "$TMP_DIR/test_e2cf_data_layout" - <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "e2cf_data_layout.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define TRAILER_VALUE UINT32_C(0x12345678)

static int failures;

static void put_le32(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)value;
	dst[1] = (uint8_t)(value >> 8);
	dst[2] = (uint8_t)(value >> 16);
	dst[3] = (uint8_t)(value >> 24);
}

static uint32_t get_le32(const uint8_t *src)
{
	return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
	       ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void set_record(uint8_t *record, uint8_t len)
{
	memset(record, 0, E2CF_DATA_REC_SIZE(len));
	record[4] = len;
}

static void expect_valid(const char *name, uint8_t *payload,
			 uint16_t payload_len, uint8_t count,
			 uint16_t expected_offset)
{
	uint16_t trailer_offset = UINT16_MAX;

	if (!e2cf_data_trailer_offset(payload, payload_len, count,
				      &trailer_offset)) {
		fprintf(stderr, "FAIL: %s rejected\n", name);
		failures++;
		return;
	}
	if (trailer_offset != expected_offset) {
		fprintf(stderr, "FAIL: %s offset=%u expected=%u\n", name,
			(unsigned int)trailer_offset,
			(unsigned int)expected_offset);
		failures++;
		return;
	}
	if (get_le32(payload + trailer_offset) != TRAILER_VALUE) {
		fprintf(stderr, "FAIL: %s trailer=0x%08x\n", name,
			(unsigned int)get_le32(payload + trailer_offset));
		failures++;
	}
}

static void expect_invalid(const char *name, const uint8_t *payload,
			   uint16_t payload_len, uint8_t count)
{
	uint16_t trailer_offset = UINT16_MAX;

	if (e2cf_data_trailer_offset(payload, payload_len, count,
				     &trailer_offset)) {
		fprintf(stderr, "FAIL: %s accepted at offset=%u\n", name,
			(unsigned int)trailer_offset);
		failures++;
	}
}

int main(void)
{
	uint8_t len0[40] = { 0 };
	uint8_t len8[40] = { 0 };
	uint8_t len20[40] = { 0 };
	uint8_t mixed[52] = { 0 };
	uint8_t short_header[7] = { 0 };
	uint8_t short_body[15] = { 0 };
	uint8_t short_trailer[19] = { 0 };
	uint8_t bad_count[20] = { 0 };
	uint16_t second_offset;

	set_record(len0, 0);
	put_le32(len0 + 8, TRAILER_VALUE);
	expect_valid("one len=0 record with padding", len0,
		     (uint16_t)ARRAY_SIZE(len0), 1, 8);

	set_record(len8, 8);
	put_le32(len8 + 16, TRAILER_VALUE);
	expect_valid("one len=8 record with padding", len8,
		     (uint16_t)ARRAY_SIZE(len8), 1, 16);

	set_record(len20, 20);
	put_le32(len20 + 28, TRAILER_VALUE);
	expect_valid("one len=20 record with padding", len20,
		     (uint16_t)ARRAY_SIZE(len20), 1, 28);

	set_record(mixed, 5);
	second_offset = (uint16_t)E2CF_DATA_REC_SIZE(5);
	set_record(mixed + second_offset, 20);
	put_le32(mixed + second_offset + E2CF_DATA_REC_SIZE(20),
		 TRAILER_VALUE);
	expect_valid("mixed records", mixed, (uint16_t)ARRAY_SIZE(mixed), 2,
		     (uint16_t)(E2CF_DATA_REC_SIZE(5) +
				E2CF_DATA_REC_SIZE(20)));

	expect_invalid("truncated before record header", short_header,
		       (uint16_t)ARRAY_SIZE(short_header), 1);

	short_body[4] = 5;
	expect_invalid("truncated inside padded body", short_body,
		       (uint16_t)ARRAY_SIZE(short_body), 1);

	set_record(short_trailer, 8);
	expect_invalid("truncated before trailer", short_trailer,
		       (uint16_t)ARRAY_SIZE(short_trailer), 1);

	set_record(bad_count, 8);
	put_le32(bad_count + 16, TRAILER_VALUE);
	expect_invalid("count exceeds unpadded logical records", bad_count,
		       (uint16_t)ARRAY_SIZE(bad_count), 2);

	if (failures)
		return 1;

	puts("PASS: E2CF DATA layout and trailer offset");
	return 0;
}
EOF

"$TMP_DIR/test_e2cf_data_layout"
