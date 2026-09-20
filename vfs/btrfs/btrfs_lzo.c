/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_lzo.c
 *
 * LZO1X decompression and the Btrfs framing around it.
 *
 * A compressed Btrfs extent is
 *
 *     u32 total       bytes of the whole thing including this field
 *     repeat:
 *         u32 length  bytes of the segment that follows
 *         segment     one LZO1X stream that decodes to at most one sector
 *
 * (little-endian). A segment header never straddles a sector: when fewer than
 * four bytes remain in the sector after a segment, the writer pads with zeros
 * up to the next sector and the reader skips them. Every segment is a stream
 * of its own (matches never reach into the previous segment), so distances are
 * checked against the start of the current segment's output.
 *
 * btrfs_lzo1x_decompress_safe has the semantics of Linux's
 * lzo1x_decompress_safe: it consumes exactly the input up to and including the
 * end-of-stream marker, never reads outside in[0..in_len), never writes outside
 * out[0..out_cap), and rejects a back-reference before the start of the output.
 * The state machine is the standard one: `state` is how many literals followed
 * the last match (0 to 3), or 4 after a literal run, which selects the meaning
 * of the shortest match codes.
 */

#include "btrfs_codec.h"

#define LZO_M2_MAX_OFFSET 0x0800U
#define LZO_M3_MARKER 32U
#define LZO_M4_MARKER 16U

/* Bytes of a segment header, and what a segment for one sector can take at worst. */
#define LZO_HEADER 4U
#define LZO_WORST_COMPRESS(size) ((size) + ((size) / 16U) + 64U + 3U)

static uint32_t btrfs_lzo_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

btrfs_status_t btrfs_lzo1x_decompress_safe(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *produced)
{
	const uint8_t *ip = in;
	const uint8_t *ip_end = in + in_len;
	uint8_t *op = out;
	uint8_t *op_end = out + out_cap;
	size_t t;
	size_t next;
	size_t distance;
	size_t length;
	unsigned state = 0U;

#define LZO_NEED_IP(n) do { if ((size_t)(ip_end - ip) < (size_t)(n)) goto fail; } while (0)
#define LZO_NEED_OP(n) do { if ((size_t)(op_end - op) < (size_t)(n)) goto fail; } while (0)

	*produced = 0U;

	if (in_len < 3U) return BTRFS_ERR_CORRUPT;

	if (*ip > 17U) {
		t = (size_t)*ip++ - 17U;

		if (t < 4U) {
			next = t;
			goto match_next;
		}

		state = 4U;
		LZO_NEED_IP(t);
		LZO_NEED_OP(t);
		while (t-- != 0U) *op++ = *ip++;
	}

	for (;;) {
		LZO_NEED_IP(1);
		t = *ip++;

		if (t < 16U) {
			if (state == 0U) {
				/* A literal run of t + 3 bytes; t == 0 extends through zero bytes. */
				if (t == 0U) {
					LZO_NEED_IP(1);
					while (*ip == 0U) {
						t += 255U;
						ip++;
						LZO_NEED_IP(1);
					}
					t += 15U + *ip++;
				}

				t += 3U;
				LZO_NEED_IP(t);
				LZO_NEED_OP(t);
				while (t-- != 0U) *op++ = *ip++;
				state = 4U;
				continue;
			}

			LZO_NEED_IP(1);
			next = t & 3U;

			if (state != 4U) {
				/* After 1..3 literals: a two-byte match that is close. */
				distance = 1U + (t >> 2U) + ((size_t)*ip++ << 2U);
				length = 2U;
			} else {
				/* After a literal run: a three-byte match that is far. */
				distance = 1U + LZO_M2_MAX_OFFSET + (t >> 2U) + ((size_t)*ip++ << 2U);
				length = 3U;
			}
		} else if (t >= 64U) {
			LZO_NEED_IP(1);
			next = t & 3U;
			distance = 1U + ((t >> 2U) & 7U) + ((size_t)*ip++ << 3U);
			length = (t >> 5U) - 1U + 2U;
		} else if (t >= LZO_M3_MARKER) {
			t &= 31U;
			if (t == 0U) {
				LZO_NEED_IP(1);
				while (*ip == 0U) {
					t += 255U;
					ip++;
					LZO_NEED_IP(1);
				}
				t += 31U + *ip++;
			}

			LZO_NEED_IP(2);
			next = ip[0] & 3U;
			distance = 1U + ((size_t)ip[0] >> 2U) + ((size_t)ip[1] << 6U);
			ip += 2;
			length = t + 2U;
		} else {
			size_t high = (size_t)(t & 8U) << 11U;

			t &= 7U;
			if (t == 0U) {
				LZO_NEED_IP(1);
				while (*ip == 0U) {
					t += 255U;
					ip++;
					LZO_NEED_IP(1);
				}
				t += 7U + *ip++;
			}

			LZO_NEED_IP(2);
			next = ip[0] & 3U;
			distance = high + ((size_t)ip[0] >> 2U) + ((size_t)ip[1] << 6U);
			ip += 2;

			if (distance == 0U) {
				/* The end-of-stream marker: 17, 0, 0 with nothing after it. */
				if (t != 1U || next != 0U || ip != ip_end) goto fail;
				*produced = (size_t)(op - out);
				return BTRFS_OK;
			}

			distance += 0x4000U;
			length = t + 2U;
		}

		if (distance > (size_t)(op - out)) goto fail;
		LZO_NEED_OP(length);

		while (length-- != 0U) {
			*op = *(op - distance);
			op++;
		}

match_next:
		/* The match is followed by 0..3 literals that ride in its low bits. */
		state = (unsigned)next;
		LZO_NEED_IP(next);
		LZO_NEED_OP(next);
		while (next-- != 0U) *op++ = *ip++;
	}

fail:
#undef LZO_NEED_IP
#undef LZO_NEED_OP
	return BTRFS_ERR_CORRUPT;
}

btrfs_status_t btrfs_lzo_decompress(void *ctx, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	const btrfs_codec_ctx_t *codec = ctx;
	size_t sector = codec->sectorsize;
	size_t total;
	size_t cursor = LZO_HEADER;
	size_t produced = 0U;

	if (sector < 512U || (sector & (sector - 1U)) != 0U) return BTRFS_ERR_CORRUPT;
	if (in_len < LZO_HEADER) return BTRFS_ERR_CORRUPT;

	total = btrfs_lzo_le32(in);
	if (total < LZO_HEADER || total > in_len) return BTRFS_ERR_CORRUPT;

	while (cursor < total) {
		/* A header does not straddle a sector: the rest of this one is padding. */
		size_t left_in_sector = sector - (cursor & (sector - 1U));

		if (left_in_sector < LZO_HEADER) {
			cursor += left_in_sector;
			if (cursor >= total) break;
		}

		if (total - cursor < LZO_HEADER) return BTRFS_ERR_CORRUPT;

		size_t segment = btrfs_lzo_le32(in + cursor);

		cursor += LZO_HEADER;
		if (segment == 0U || segment > LZO_WORST_COMPRESS(sector) || segment > total - cursor) return BTRFS_ERR_CORRUPT;

		/* A segment decodes to one sector at most, and never past the end of the extent. */
		size_t room = out_len - produced;
		size_t made;

		if (room > sector) room = sector;
		if (room == 0U) return BTRFS_ERR_CORRUPT;

		btrfs_status_t status = btrfs_lzo1x_decompress_safe(in + cursor, segment, out + produced, room, &made);
		if (status != BTRFS_OK) return status;
		if (made == 0U) return BTRFS_ERR_CORRUPT;

		produced += made;
		cursor += segment;
	}

	/* An extent that decodes to nothing is not an extent. The last one of a file is rounded up to a sector; the rest reads as zeros. */
	if (produced == 0U) return BTRFS_ERR_CORRUPT;
	for (size_t i = produced; i < out_len; i++) out[i] = 0U;
	return BTRFS_OK;
}
