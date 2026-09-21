/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_zlib.c
 *
 * zlib (RFC 1950) around DEFLATE (RFC 1951) for compressed Btrfs extents.
 * Btrfs stores one complete zlib stream per extent. The decoder is a
 * canonical-Huffman inflater in the style of zlib's own puff.c: no window
 * buffer (the whole output is the window), no allocation, every bit and byte
 * read is bounds-checked, and the output never grows past out_len.
 *
 * Refused as corrupt: a bad header (method, window, check bits, preset
 * dictionary), oversubscribed or incomplete Huffman codes (except the single
 * distance code zlib also accepts), a distance reaching before the start of
 * the output, output that would exceed out_len, and an Adler-32 mismatch. Bytes
 * after the Adler-32 are the sector padding and are ignored. A stream that ends
 * short of out_len is zero-filled to out_len, as Linux does: the last extent of
 * a file is rounded up to a sector but only the file's bytes were compressed.
 */

#include "btrfs_codec.h"

#define INFLATE_MAX_BITS 15U
#define INFLATE_LIT_CODES 288U
#define INFLATE_MAX_LCODES 286U
#define INFLATE_MAX_DCODES 30U

typedef struct {
	const uint8_t *in;
	size_t in_len;
	size_t in_pos;
	uint32_t bitbuf;
	unsigned bitcnt;
	uint8_t *out;
	size_t out_len;
	size_t out_pos;
} btrfs_inflate_t;

typedef struct {
	uint16_t count[INFLATE_MAX_BITS + 1U];   /* codes of each length */
	uint16_t symbol[INFLATE_LIT_CODES];      /* symbols ordered by code */
} btrfs_huffman_t;

/* Up to 16 bits (the largest DEFLATE field is 13). */
static bool btrfs_inflate_bits(btrfs_inflate_t *s, unsigned need, uint32_t *value)
{
	while (s->bitcnt < need) {
		if (s->in_pos >= s->in_len) return false;
		s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
		s->bitcnt += 8U;
	}

	*value = s->bitbuf & ((1U << need) - 1U);
	s->bitbuf >>= need;
	s->bitcnt -= need;
	return true;
}

/*
 * Build a decoding table from code lengths. Returns the number of unused code
 * space: 0 for a complete code, > 0 for an incomplete one, < 0 if oversubscribed.
 */
static int btrfs_inflate_construct(btrfs_huffman_t *h, const uint8_t *lengths, unsigned n)
{
	uint16_t offsets[INFLATE_MAX_BITS + 1U];
	int left = 1;

	for (unsigned len = 0U; len <= INFLATE_MAX_BITS; len++) h->count[len] = 0U;
	for (unsigned symbol = 0U; symbol < n; symbol++) h->count[lengths[symbol]]++;
	if (h->count[0] == n) return 0;

	for (unsigned len = 1U; len <= INFLATE_MAX_BITS; len++) {
		left <<= 1;
		left -= (int)h->count[len];
		if (left < 0) return left;
	}

	offsets[1] = 0U;
	for (unsigned len = 1U; len < INFLATE_MAX_BITS; len++) offsets[len + 1U] = (uint16_t)(offsets[len] + h->count[len]);
	for (unsigned symbol = 0U; symbol < n; symbol++) {
		if (lengths[symbol] != 0U) h->symbol[offsets[lengths[symbol]]++] = (uint16_t)symbol;
	}

	return left;
}

/* The next symbol, or -1 on a code that is not in the table or on running out of input. */
static int btrfs_inflate_decode(btrfs_inflate_t *s, const btrfs_huffman_t *h)
{
	int code = 0;
	int first = 0;
	int index = 0;

	for (unsigned len = 1U; len <= INFLATE_MAX_BITS; len++) {
		uint32_t bit;

		if (!btrfs_inflate_bits(s, 1U, &bit)) return -1;

		code |= (int)bit;
		int count = (int)h->count[len];
		if (code - count < first) return (int)h->symbol[index + (code - first)];

		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}

	return -1;
}

static const uint16_t btrfs_inflate_length_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t btrfs_inflate_length_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t btrfs_inflate_dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t btrfs_inflate_dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* The literal/length + distance symbols of one Huffman-coded block. */
static bool btrfs_inflate_codes(btrfs_inflate_t *s, const btrfs_huffman_t *lencode, const btrfs_huffman_t *distcode)
{
	for (;;) {
		int symbol = btrfs_inflate_decode(s, lencode);
		uint32_t extra;

		if (symbol < 0) return false;

		if (symbol < 256) {
			if (s->out_pos >= s->out_len) return false;
			s->out[s->out_pos++] = (uint8_t)symbol;
			continue;
		}

		if (symbol == 256) return true;

		symbol -= 257;
		if (symbol >= 29) return false;

		if (!btrfs_inflate_bits(s, btrfs_inflate_length_extra[symbol], &extra)) return false;
		size_t length = (size_t)btrfs_inflate_length_base[symbol] + extra;

		int dsymbol = btrfs_inflate_decode(s, distcode);
		if (dsymbol < 0 || dsymbol >= (int)INFLATE_MAX_DCODES) return false;

		if (!btrfs_inflate_bits(s, btrfs_inflate_dist_extra[dsymbol], &extra)) return false;
		size_t distance = (size_t)btrfs_inflate_dist_base[dsymbol] + extra;

		if (distance > s->out_pos) return false;
		if (length > s->out_len - s->out_pos) return false;

		for (size_t i = 0U; i < length; i++) {
			s->out[s->out_pos] = s->out[s->out_pos - distance];
			s->out_pos++;
		}
	}
}

static bool btrfs_inflate_stored(btrfs_inflate_t *s)
{
	/* Whatever is left of the current byte is dropped. */
	s->bitbuf = 0U;
	s->bitcnt = 0U;

	if (s->in_len - s->in_pos < 4U) return false;

	uint32_t length = (uint32_t)s->in[s->in_pos] | ((uint32_t)s->in[s->in_pos + 1U] << 8U);
	uint32_t inverse = (uint32_t)s->in[s->in_pos + 2U] | ((uint32_t)s->in[s->in_pos + 3U] << 8U);

	s->in_pos += 4U;
	if (length != (~inverse & 0xffffU)) return false;
	if (length > s->in_len - s->in_pos || length > s->out_len - s->out_pos) return false;

	for (uint32_t i = 0U; i < length; i++) s->out[s->out_pos++] = s->in[s->in_pos++];
	return true;
}

static bool btrfs_inflate_fixed(btrfs_inflate_t *s)
{
	static const uint8_t distance_lengths[INFLATE_MAX_DCODES] = { 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5 };
	btrfs_huffman_t lencode;
	btrfs_huffman_t distcode;
	uint8_t lengths[INFLATE_LIT_CODES];
	unsigned symbol = 0U;

	for (; symbol < 144U; symbol++) lengths[symbol] = 8U;
	for (; symbol < 256U; symbol++) lengths[symbol] = 9U;
	for (; symbol < 280U; symbol++) lengths[symbol] = 7U;
	for (; symbol < INFLATE_LIT_CODES; symbol++) lengths[symbol] = 8U;

	btrfs_inflate_construct(&lencode, lengths, INFLATE_LIT_CODES);
	btrfs_inflate_construct(&distcode, distance_lengths, INFLATE_MAX_DCODES);
	return btrfs_inflate_codes(s, &lencode, &distcode);
}

static bool btrfs_inflate_dynamic(btrfs_inflate_t *s)
{
	static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	btrfs_huffman_t lencode;
	btrfs_huffman_t distcode;
	uint8_t lengths[INFLATE_MAX_LCODES + INFLATE_MAX_DCODES + 20U];
	uint32_t value;

	if (!btrfs_inflate_bits(s, 5U, &value)) return false;
	unsigned nlen = value + 257U;
	if (!btrfs_inflate_bits(s, 5U, &value)) return false;
	unsigned ndist = value + 1U;
	if (!btrfs_inflate_bits(s, 4U, &value)) return false;
	unsigned ncode = value + 4U;

	if (nlen > INFLATE_MAX_LCODES || ndist > INFLATE_MAX_DCODES) return false;

	/* The code that codes the code lengths: must be complete. */
	unsigned index = 0U;
	for (; index < ncode; index++) {
		if (!btrfs_inflate_bits(s, 3U, &value)) return false;
		lengths[order[index]] = (uint8_t)value;
	}
	for (; index < 19U; index++) lengths[order[index]] = 0U;

	if (btrfs_inflate_construct(&lencode, lengths, 19U) != 0) return false;

	index = 0U;
	while (index < nlen + ndist) {
		int symbol = btrfs_inflate_decode(s, &lencode);
		uint8_t len = 0U;
		unsigned repeat;

		if (symbol < 0) return false;

		if (symbol < 16) {
			lengths[index++] = (uint8_t)symbol;
			continue;
		}

		if (symbol == 16) {
			if (index == 0U) return false;
			len = lengths[index - 1U];
			if (!btrfs_inflate_bits(s, 2U, &value)) return false;
			repeat = 3U + value;
		} else if (symbol == 17) {
			if (!btrfs_inflate_bits(s, 3U, &value)) return false;
			repeat = 3U + value;
		} else {
			if (!btrfs_inflate_bits(s, 7U, &value)) return false;
			repeat = 11U + value;
		}

		if (index + repeat > nlen + ndist) return false;
		while (repeat-- != 0U) lengths[index++] = len;
	}

	/* The end-of-block symbol must be codable. */
	if (lengths[256] == 0U) return false;

	int err = btrfs_inflate_construct(&lencode, lengths, nlen);
	if (err < 0 || (err > 0 && nlen != (unsigned)lencode.count[0] + lencode.count[1])) return false;

	err = btrfs_inflate_construct(&distcode, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist != (unsigned)distcode.count[0] + distcode.count[1])) return false;

	return btrfs_inflate_codes(s, &lencode, &distcode);
}

static uint32_t btrfs_adler32(const uint8_t *data, size_t length)
{
	uint32_t a = 1U;
	uint32_t b = 0U;

	while (length != 0U) {
		size_t run = length < 5552U ? length : 5552U;

		length -= run;
		while (run-- != 0U) {
			a += *data++;
			b += a;
		}

		a %= 65521U;
		b %= 65521U;
	}

	return (b << 16U) | a;
}

btrfs_status_t btrfs_zlib_decompress(void *ctx, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	btrfs_inflate_t s;
	bool last;

	(void)ctx;

	if (in_len < 2U + 4U) return BTRFS_ERR_CORRUPT;

	/* RFC 1950: deflate, window of at most 32 KiB, header check bits, no preset dictionary. */
	if ((in[0] & 0x0fU) != 8U || (in[0] >> 4U) > 7U) return BTRFS_ERR_CORRUPT;
	if ((((unsigned)in[0] << 8U) | in[1]) % 31U != 0U || (in[1] & 0x20U) != 0U) return BTRFS_ERR_CORRUPT;

	s.in = in;
	s.in_len = in_len;
	s.in_pos = 2U;
	s.bitbuf = 0U;
	s.bitcnt = 0U;
	s.out = out;
	s.out_len = out_len;
	s.out_pos = 0U;

	do {
		uint32_t value;
		bool ok;

		if (!btrfs_inflate_bits(&s, 1U, &value)) return BTRFS_ERR_CORRUPT;
		last = value != 0U;

		if (!btrfs_inflate_bits(&s, 2U, &value)) return BTRFS_ERR_CORRUPT;

		switch (value) {
		case 0U:
			ok = btrfs_inflate_stored(&s);
			break;
		case 1U:
			ok = btrfs_inflate_fixed(&s);
			break;
		case 2U:
			ok = btrfs_inflate_dynamic(&s);
			break;
		default:
			ok = false;
			break;
		}

		if (!ok) return BTRFS_ERR_CORRUPT;
	} while (!last);

	/* Byte-align, then the big-endian Adler-32 of the output. */
	s.bitbuf = 0U;
	s.bitcnt = 0U;
	if (in_len - s.in_pos < 4U) return BTRFS_ERR_CORRUPT;

	uint32_t adler = ((uint32_t)in[s.in_pos] << 24U) | ((uint32_t)in[s.in_pos + 1U] << 16U) | ((uint32_t)in[s.in_pos + 2U] << 8U) | in[s.in_pos + 3U];
	if (adler != btrfs_adler32(out, s.out_pos)) return BTRFS_ERR_CORRUPT;

	if (s.out_pos == 0U) return BTRFS_ERR_CORRUPT;

	/* Btrfs rounds the last extent up to a sector and the stream only holds the file's bytes. */
	for (size_t i = s.out_pos; i < out_len; i++) out[i] = 0U;
	return BTRFS_OK;
}
