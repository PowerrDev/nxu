/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_zstd.c
 *
 * A Zstandard (RFC 8878) frame decoder for compressed Btrfs extents. Btrfs
 * writes one frame per extent, produced with a window of at most 128 KiB.
 *
 * The whole decoded extent is the output buffer, so it doubles as the window:
 * a match is a copy from earlier in `out`, and no history buffer exists.
 * Work memory is one allocation through the injected environment: the FSE and
 * Huffman tables (about 11 KiB) plus room for one block's literals (at most
 * min(out_len, 128 KiB)).
 *
 * Supported: skippable frames, raw / RLE / compressed blocks, raw / RLE /
 * Huffman (one and four streams, FSE-compressed or direct weights, treeless
 * "repeat") literals, sequences with predefined / RLE / FSE / repeat tables,
 * repeat offsets, the optional content checksum (verified). Not supported (all
 * refused as corrupt): dictionaries, windows above 128 KiB, reserved bits and
 * block types.
 *
 * Hardening: every read from the input goes through a reader that knows the
 * input's end; every write knows the output's end (out_len is a hard cap, the
 * decompression-bomb limit); a back-reference must lie inside what this frame
 * has produced so far; table descriptions are checked to be complete
 * distributions; backward bitstreams must be consumed to exactly the last bit;
 * no loop is unbounded (each iteration consumes input or output).
 */

#include "btrfs_codec.h"

#include <string.h>

#define ZSTD_MAGIC 0xFD2FB528U
#define ZSTD_SKIPPABLE_MASK 0xFFFFFFF0U
#define ZSTD_SKIPPABLE_MAGIC 0x184D2A50U

#define ZSTD_BLOCK_MAX (128U * 1024U)

#define ZSTD_LL_MAX_SYMBOL 35U
#define ZSTD_OF_MAX_SYMBOL 31U
#define ZSTD_ML_MAX_SYMBOL 52U
#define ZSTD_LL_MAX_LOG 9U
#define ZSTD_OF_MAX_LOG 8U
#define ZSTD_ML_MAX_LOG 9U
#define ZSTD_HUF_FSE_MAX_LOG 6U
#define ZSTD_HUF_MAX_BITS 11U
#define ZSTD_FSE_TABLE_ENTRIES 512U
#define ZSTD_MAX_SYMBOLS (ZSTD_ML_MAX_SYMBOL + 1U)

/* ---- bit readers ---------------------------------------------------------------------------- */

/* n <= 32 bits starting at bit offset `lo` (LSB first) of data[0..nbytes); bytes past the end read as 0. */
static uint64_t btrfs_zstd_gather(const uint8_t *data, size_t nbytes, uint64_t lo, unsigned n)
{
	uint64_t index = lo >> 3U;
	uint64_t value = 0U;

	for (unsigned i = 0U; i < 5U; i++) {
		if (index + i < nbytes) value |= (uint64_t)data[index + i] << (8U * i);
	}

	return (value >> (lo & 7U)) & ((1ULL << n) - 1ULL);
}

/* A forward reader, for the packed FSE table descriptions. */
typedef struct {
	const uint8_t *data;
	size_t nbytes;
	uint64_t bitpos;
} btrfs_zfwd_t;

static uint32_t btrfs_zfwd_peek(const btrfs_zfwd_t *f, unsigned n)
{
	return (uint32_t)btrfs_zstd_gather(f->data, f->nbytes, f->bitpos, n);
}

/*
 * A backward reader: the stream is read from its last bit to its first, the
 * last byte carries a 1 marker above the data. bits is what is left; it goes
 * negative when more was read than exists (missing bits read as zeros), which
 * every user treats as corruption unless it is the documented end of a stream.
 */
typedef struct {
	const uint8_t *data;
	size_t nbytes;
	int64_t bits;
} btrfs_zrev_t;

static bool btrfs_zrev_init(btrfs_zrev_t *r, const uint8_t *data, size_t nbytes)
{
	if (nbytes == 0U || data[nbytes - 1U] == 0U) return false;

	unsigned top = 7U;
	while ((data[nbytes - 1U] & (1U << top)) == 0U) top--;

	r->data = data;
	r->nbytes = nbytes;
	r->bits = (int64_t)(nbytes - 1U) * 8 + (int64_t)top;
	return true;
}

/* The next n <= 32 bits; the earliest is the most significant. Not consumed. */
static uint32_t btrfs_zrev_peek(const btrfs_zrev_t *r, unsigned n)
{
	if (n == 0U) return 0U;

	int64_t lo = r->bits - (int64_t)n;

	if (lo >= 0) return (uint32_t)btrfs_zstd_gather(r->data, r->nbytes, (uint64_t)lo, n);
	if (r->bits <= 0) return 0U;

	unsigned have = (unsigned)r->bits;
	return (uint32_t)(btrfs_zstd_gather(r->data, r->nbytes, 0U, have) << (n - have));
}

static uint32_t btrfs_zrev_read(btrfs_zrev_t *r, unsigned n)
{
	uint32_t value = btrfs_zrev_peek(r, n);

	r->bits -= (int64_t)n;
	return value;
}

/* ---- FSE ---------------------------------------------------------------------------------- */

typedef struct {
	uint16_t base;     /* the next state is base + the next nbits bits */
	uint8_t symbol;
	uint8_t nbits;
} btrfs_zfse_entry_t;

typedef struct {
	btrfs_zfse_entry_t entry[ZSTD_FSE_TABLE_ENTRIES];
	unsigned log;      /* accuracy log; the table has 1 << log entries */
	bool valid;        /* a table exists for "repeat" to reuse */
} btrfs_zfse_t;

static unsigned btrfs_zstd_highbit(uint32_t value)
{
	unsigned bit = 0U;

	while (value > 1U) {
		value >>= 1U;
		bit++;
	}

	return bit;
}

/*
 * Read a table description (RFC 8878 4.1.1) into normalized counts. Symbols are
 * numbered 0..*count-1 and must not exceed max_symbol. On success *used is the
 * number of bytes the description took.
 */
static bool btrfs_zstd_read_counts(const uint8_t *data, size_t nbytes, int16_t *norm, unsigned max_symbol, unsigned max_log, unsigned *log_out, unsigned *count_out, size_t *used)
{
	btrfs_zfwd_t f = { data, nbytes, 0U };
	unsigned log = 5U + btrfs_zfwd_peek(&f, 4U);

	f.bitpos = 4U;
	if (log > max_log) return false;

	int remaining = (int)(1U << log) + 1;
	unsigned threshold = 1U << log;
	unsigned nbits = log + 1U;
	unsigned symbol = 0U;
	bool previous_zero = false;

	while (remaining > 1 && symbol <= max_symbol) {
		if (previous_zero) {
			/* Runs of zero-probability symbols: 2-bit repeat counts, 3 meaning "and more". */
			for (;;) {
				unsigned repeat = btrfs_zfwd_peek(&f, 2U);

				f.bitpos += 2U;
				symbol += repeat;
				if (symbol > max_symbol) return false;
				for (unsigned z = symbol - repeat; z < symbol; z++) norm[z] = 0;
				if (repeat != 3U) break;
				if (f.bitpos > (uint64_t)nbytes * 8U) return false;
			}
		}

		int max = (int)(2U * threshold - 1U) - remaining;
		uint32_t bits = btrfs_zfwd_peek(&f, nbits);
		int count;

		if ((int)(bits & (threshold - 1U)) < max) {
			count = (int)(bits & (threshold - 1U));
			f.bitpos += nbits - 1U;
		} else {
			count = (int)(bits & (2U * threshold - 1U));
			if (count >= (int)threshold) count -= max;
			f.bitpos += nbits;
		}

		count--;
		remaining -= count < 0 ? -count : count;
		if (remaining < 1) return false;

		norm[symbol++] = (int16_t)count;
		previous_zero = count == 0;

		while (remaining < (int)threshold) {
			nbits--;
			threshold >>= 1U;
		}

		if (f.bitpos > (uint64_t)nbytes * 8U) return false;
	}

	if (remaining != 1 || f.bitpos > (uint64_t)nbytes * 8U) return false;

	*log_out = log;
	*count_out = symbol;
	*used = (size_t)((f.bitpos + 7U) >> 3U);
	return true;
}

/* The decoding table for a set of normalized counts summing to 1 << log (-1: probability "less than 1"). */
static bool btrfs_zstd_build_fse(btrfs_zfse_t *t, const int16_t *norm, unsigned count, unsigned log)
{
	uint16_t next[ZSTD_MAX_SYMBOLS];
	unsigned size = 1U << log;
	unsigned high = size - 1U;
	unsigned position = 0U;
	unsigned step = (size >> 1U) + (size >> 3U) + 3U;

	if (count > ZSTD_MAX_SYMBOLS || log > 9U) return false;

	for (unsigned s = 0U; s < count; s++) {
		if (norm[s] == -1) {
			t->entry[high--].symbol = (uint8_t)s;
			next[s] = 1U;
		} else {
			next[s] = (uint16_t)norm[s];
		}
	}

	for (unsigned s = 0U; s < count; s++) {
		for (int i = 0; i < norm[s]; i++) {
			t->entry[position].symbol = (uint8_t)s;
			position = (position + step) & (size - 1U);
			while (position > high) position = (position + step) & (size - 1U);
		}
	}

	if (position != 0U) return false;

	for (unsigned u = 0U; u < size; u++) {
		unsigned state = next[t->entry[u].symbol]++;
		unsigned bits = log - btrfs_zstd_highbit(state);

		t->entry[u].nbits = (uint8_t)bits;
		t->entry[u].base = (uint16_t)((state << bits) - size);
	}

	t->log = log;
	t->valid = true;
	return true;
}

/* A table with one symbol that costs no bits (RLE mode). */
static void btrfs_zstd_build_rle(btrfs_zfse_t *t, uint8_t symbol)
{
	t->entry[0].base = 0U;
	t->entry[0].symbol = symbol;
	t->entry[0].nbits = 0U;
	t->log = 0U;
	t->valid = true;
}

static const int16_t btrfs_zstd_ll_default[36] = {
	4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1
};
static const int16_t btrfs_zstd_of_default[29] = {
	1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1
};
static const int16_t btrfs_zstd_ml_default[53] = {
	1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1
};

/* Literal length and match length codes: baseline and extra bits. */
static const uint32_t btrfs_zstd_ll_base[36] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
};
static const uint8_t btrfs_zstd_ll_bits[36] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};
static const uint32_t btrfs_zstd_ml_base[53] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
	35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539
};
static const uint8_t btrfs_zstd_ml_bits[53] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};

/* ---- decoder state ------------------------------------------------------------------------- */

typedef struct {
	uint8_t symbol;
	uint8_t nbits;
} btrfs_zhuf_entry_t;

typedef struct {
	btrfs_zhuf_entry_t table[1U << ZSTD_HUF_MAX_BITS];
	unsigned max_bits;
	bool valid;
} btrfs_zhuf_t;

typedef struct {
	btrfs_zfse_t ll;
	btrfs_zfse_t of;
	btrfs_zfse_t ml;
	btrfs_zhuf_t huf;
	uint32_t rep[3];
	uint8_t *literals;       /* room for one block's literals */
	size_t literals_cap;
} btrfs_zstd_state_t;

/* ---- literals -------------------------------------------------------------------------------- */

/* Build the Huffman decoding table from the weights of symbols 0..n-1 (the last symbol's weight is implied). */
static bool btrfs_zstd_build_huffman(btrfs_zhuf_t *h, uint8_t *weights, unsigned n)
{
	uint32_t sum = 0U;
	unsigned counts[ZSTD_HUF_MAX_BITS + 2U];
	uint32_t start[ZSTD_HUF_MAX_BITS + 2U];

	if (n == 0U || n > 255U) return false;

	for (unsigned i = 0U; i < n; i++) {
		if (weights[i] > ZSTD_HUF_MAX_BITS) return false;
		if (weights[i] != 0U) sum += 1U << (weights[i] - 1U);
	}

	if (sum == 0U) return false;

	unsigned max_bits = btrfs_zstd_highbit(sum) + 1U;
	if (max_bits > ZSTD_HUF_MAX_BITS) return false;

	uint32_t total = 1U << max_bits;
	uint32_t rest = total - sum;

	if (rest == 0U || (rest & (rest - 1U)) != 0U) return false;

	weights[n] = (uint8_t)(btrfs_zstd_highbit(rest) + 1U);
	n++;

	for (unsigned w = 0U; w <= ZSTD_HUF_MAX_BITS + 1U; w++) counts[w] = 0U;
	for (unsigned i = 0U; i < n; i++) counts[weights[i]]++;

	/* Codes of the smallest weight (longest) come first in the table, in symbol order. */
	uint32_t slot = 0U;
	for (unsigned w = 1U; w <= max_bits; w++) {
		start[w] = slot;
		slot += (uint32_t)counts[w] << (w - 1U);
	}

	for (unsigned i = 0U; i < n; i++) {
		unsigned w = weights[i];

		if (w == 0U) continue;

		uint32_t span = 1U << (w - 1U);
		for (uint32_t k = 0U; k < span; k++) {
			h->table[start[w] + k].symbol = (uint8_t)i;
			h->table[start[w] + k].nbits = (uint8_t)(max_bits + 1U - w);
		}
		start[w] += span;
	}

	h->max_bits = max_bits;
	h->valid = true;
	return true;
}

/* Decode weights coded as an FSE stream with two interleaved states (RFC 8878 4.2.1.2). */
static bool btrfs_zstd_fse_weights(const uint8_t *data, size_t nbytes, uint8_t *weights, unsigned *count)
{
	btrfs_zfse_t table;
	int16_t norm[16];
	unsigned log;
	unsigned nsym;
	size_t used;

	if (!btrfs_zstd_read_counts(data, nbytes, norm, 12U, ZSTD_HUF_FSE_MAX_LOG, &log, &nsym, &used)) return false;
	if (!btrfs_zstd_build_fse(&table, norm, nsym, log)) return false;

	btrfs_zrev_t r;
	if (!btrfs_zrev_init(&r, data + used, nbytes - used)) return false;

	unsigned s1 = btrfs_zrev_read(&r, log);
	unsigned s2 = btrfs_zrev_read(&r, log);
	unsigned n = 0U;

	if (r.bits < 0) return false;

	/* Symbols alternate between the states until reading a state's update runs past the start. */
	for (;;) {
		if (n + 2U > 255U) return false;

		weights[n++] = table.entry[s1].symbol;
		s1 = table.entry[s1].base + btrfs_zrev_read(&r, table.entry[s1].nbits);
		if (r.bits < 0) {
			weights[n++] = table.entry[s2].symbol;
			break;
		}

		weights[n++] = table.entry[s2].symbol;
		s2 = table.entry[s2].base + btrfs_zrev_read(&r, table.entry[s2].nbits);
		if (r.bits < 0) {
			weights[n++] = table.entry[s1].symbol;
			break;
		}
	}

	*count = n;
	return true;
}

/* Read the Huffman tree description of a compressed-literals section; *used is its size. */
static bool btrfs_zstd_read_huffman(btrfs_zhuf_t *h, const uint8_t *data, size_t nbytes, size_t *used)
{
	uint8_t weights[256];
	unsigned n;

	if (nbytes < 1U) return false;

	unsigned header = data[0];

	if (header >= 128U) {
		n = header - 127U;

		size_t bytes = ((size_t)n + 1U) / 2U;
		if (bytes > nbytes - 1U) return false;

		for (unsigned i = 0U; i < n; i++) {
			uint8_t byte = data[1U + i / 2U];
			weights[i] = (i & 1U) == 0U ? (uint8_t)(byte >> 4U) : (uint8_t)(byte & 15U);
		}

		*used = 1U + bytes;
	} else {
		if (header == 0U || header > nbytes - 1U) return false;
		if (!btrfs_zstd_fse_weights(data + 1U, header, weights, &n)) return false;

		*used = 1U + header;
	}

	return btrfs_zstd_build_huffman(h, weights, n);
}

static bool btrfs_zstd_huffman_stream(const btrfs_zhuf_t *h, const uint8_t *data, size_t nbytes, uint8_t *out, size_t count)
{
	btrfs_zrev_t r;

	if (!btrfs_zrev_init(&r, data, nbytes)) return false;

	for (size_t i = 0U; i < count; i++) {
		btrfs_zhuf_entry_t e = h->table[btrfs_zrev_peek(&r, h->max_bits)];

		r.bits -= e.nbits;
		if (r.bits < 0) return false;
		out[i] = e.symbol;
	}

	return r.bits == 0;
}

/*
 * The literals section of a compressed block. On success *lits points at the
 * decoded literals (into `in` for raw ones, else into the state's buffer),
 * *lit_size is how many, and *used how many input bytes the section took.
 */
static bool btrfs_zstd_literals(btrfs_zstd_state_t *z, const uint8_t *in, size_t nbytes, size_t room, const uint8_t **lits, size_t *lit_size, size_t *used)
{
	if (nbytes < 1U) return false;

	unsigned type = in[0] & 3U;
	unsigned format = (in[0] >> 2U) & 3U;
	size_t regen;
	size_t compressed = 0U;
	size_t header;

	if (type == 0U || type == 1U) {
		if (format == 0U || format == 2U) {
			header = 1U;
			regen = in[0] >> 3U;
		} else if (format == 1U) {
			if (nbytes < 2U) return false;
			header = 2U;
			regen = ((size_t)in[0] >> 4U) + ((size_t)in[1] << 4U);
		} else {
			if (nbytes < 3U) return false;
			header = 3U;
			regen = ((size_t)in[0] >> 4U) + ((size_t)in[1] << 4U) + ((size_t)in[2] << 12U);
		}
	} else {
		unsigned bits = format == 2U ? 14U : (format == 3U ? 18U : 10U);
		uint64_t value;

		header = format == 0U || format == 1U ? 3U : (format == 2U ? 4U : 5U);
		if (nbytes < header) return false;

		value = 0U;
		for (size_t i = 0U; i < header; i++) value |= (uint64_t)in[i] << (8U * i);
		regen = (size_t)((value >> 4U) & ((1ULL << bits) - 1ULL));
		compressed = (size_t)((value >> (4U + bits)) & ((1ULL << bits) - 1ULL));

		if (compressed > nbytes - header) return false;
	}

	if (regen > ZSTD_BLOCK_MAX || regen > room) return false;

	if (type == 0U) {
		if (regen > nbytes - header) return false;
		*lits = in + header;
		*lit_size = regen;
		*used = header + regen;
		return true;
	}

	if (regen > z->literals_cap) return false;

	if (type == 1U) {
		if (nbytes - header < 1U) return false;
		memset(z->literals, in[header], regen);
		*lits = z->literals;
		*lit_size = regen;
		*used = header + 1U;
		return true;
	}

	/* Compressed (2) or treeless (3): a Huffman tree (unless repeated) and one or four streams. */
	const uint8_t *body = in + header;
	size_t body_size = compressed;

	if (type == 2U) {
		size_t tree;

		z->huf.valid = false;
		if (!btrfs_zstd_read_huffman(&z->huf, body, body_size, &tree)) return false;
		body += tree;
		body_size -= tree;
	} else if (!z->huf.valid) {
		return false;
	}

	if (format == 0U) {
		if (!btrfs_zstd_huffman_stream(&z->huf, body, body_size, z->literals, regen)) return false;
	} else {
		if (body_size < 6U) return false;

		size_t s1 = (size_t)body[0] | ((size_t)body[1] << 8U);
		size_t s2 = (size_t)body[2] | ((size_t)body[3] << 8U);
		size_t s3 = (size_t)body[4] | ((size_t)body[5] << 8U);

		body += 6;
		body_size -= 6U;

		if (s1 > body_size || s2 > body_size - s1 || s3 > body_size - s1 - s2) return false;

		size_t s4 = body_size - s1 - s2 - s3;
		size_t part = (regen + 3U) / 4U;

		if (part * 3U > regen) return false;

		size_t last = regen - part * 3U;

		if (!btrfs_zstd_huffman_stream(&z->huf, body, s1, z->literals, part)) return false;
		if (!btrfs_zstd_huffman_stream(&z->huf, body + s1, s2, z->literals + part, part)) return false;
		if (!btrfs_zstd_huffman_stream(&z->huf, body + s1 + s2, s3, z->literals + part * 2U, part)) return false;
		if (!btrfs_zstd_huffman_stream(&z->huf, body + s1 + s2 + s3, s4, z->literals + part * 3U, last)) return false;
	}

	*lits = z->literals;
	*lit_size = regen;
	*used = header + compressed;
	return true;
}

/* ---- sequences ------------------------------------------------------------------------------- */

/* Set up one of the three tables for this block from its mode; *used advances over the description. */
static bool btrfs_zstd_seq_table(btrfs_zfse_t *table, unsigned mode, const uint8_t *data, size_t nbytes, size_t *used, unsigned max_symbol, unsigned max_log, const int16_t *defaults, unsigned default_count, unsigned default_log)
{
	int16_t norm[ZSTD_MAX_SYMBOLS];
	unsigned log;
	unsigned count;
	size_t bytes;

	*used = 0U;

	switch (mode) {
	case 0U:
		return btrfs_zstd_build_fse(table, defaults, default_count, default_log);
	case 1U:
		if (nbytes < 1U || data[0] > max_symbol) return false;
		btrfs_zstd_build_rle(table, data[0]);
		*used = 1U;
		return true;
	case 2U:
		if (!btrfs_zstd_read_counts(data, nbytes, norm, max_symbol, max_log, &log, &count, &bytes)) return false;
		if (!btrfs_zstd_build_fse(table, norm, count, log)) return false;
		*used = bytes;
		return true;
	default:
		return table->valid;
	}
}

/* Run one block's sequences and the trailing literals; *op advances, never past out_end. */
static bool btrfs_zstd_sequences(btrfs_zstd_state_t *z, const uint8_t *in, size_t nbytes, const uint8_t *lits, size_t lit_size, uint8_t *frame_start, uint8_t **op, uint8_t *out_end)
{
	if (nbytes < 1U) return false;

	size_t count = in[0];
	size_t pos = 1U;
	uint8_t *out = *op;

	if (count >= 128U && count < 255U) {
		if (nbytes < 2U) return false;
		count = ((count - 128U) << 8U) + in[1];
		pos = 2U;
	} else if (count == 255U) {
		if (nbytes < 3U) return false;
		count = (size_t)in[1] + ((size_t)in[2] << 8U) + 0x7F00U;
		pos = 3U;
	}

	size_t lit_pos = 0U;

	if (count != 0U) {
		if (nbytes - pos < 1U) return false;

		unsigned modes = in[pos++];
		size_t used;

		if ((modes & 3U) != 0U) return false;

		if (!btrfs_zstd_seq_table(&z->ll, modes >> 6U, in + pos, nbytes - pos, &used, ZSTD_LL_MAX_SYMBOL, ZSTD_LL_MAX_LOG, btrfs_zstd_ll_default, 36U, 6U)) return false;
		pos += used;
		if (!btrfs_zstd_seq_table(&z->of, (modes >> 4U) & 3U, in + pos, nbytes - pos, &used, ZSTD_OF_MAX_SYMBOL, ZSTD_OF_MAX_LOG, btrfs_zstd_of_default, 29U, 5U)) return false;
		pos += used;
		if (!btrfs_zstd_seq_table(&z->ml, (modes >> 2U) & 3U, in + pos, nbytes - pos, &used, ZSTD_ML_MAX_SYMBOL, ZSTD_ML_MAX_LOG, btrfs_zstd_ml_default, 53U, 6U)) return false;
		pos += used;

		btrfs_zrev_t r;
		if (!btrfs_zrev_init(&r, in + pos, nbytes - pos)) return false;

		unsigned ll_state = btrfs_zrev_read(&r, z->ll.log);
		unsigned of_state = btrfs_zrev_read(&r, z->of.log);
		unsigned ml_state = btrfs_zrev_read(&r, z->ml.log);

		if (r.bits < 0) return false;

		for (size_t i = 0U; i < count; i++) {
			if (ll_state >= (1U << z->ll.log) || of_state >= (1U << z->of.log) || ml_state >= (1U << z->ml.log)) return false;

			btrfs_zfse_entry_t lle = z->ll.entry[ll_state];
			btrfs_zfse_entry_t ofe = z->of.entry[of_state];
			btrfs_zfse_entry_t mle = z->ml.entry[ml_state];

			if (lle.symbol > ZSTD_LL_MAX_SYMBOL || ofe.symbol > ZSTD_OF_MAX_SYMBOL || mle.symbol > ZSTD_ML_MAX_SYMBOL) return false;

			uint32_t offset_value = (1U << ofe.symbol) + btrfs_zrev_read(&r, ofe.symbol);
			uint32_t match = btrfs_zstd_ml_base[mle.symbol] + btrfs_zrev_read(&r, btrfs_zstd_ml_bits[mle.symbol]);
			uint32_t literal = btrfs_zstd_ll_base[lle.symbol] + btrfs_zrev_read(&r, btrfs_zstd_ll_bits[lle.symbol]);

			if (r.bits < 0) return false;

			if (i + 1U < count) {
				ll_state = lle.base + btrfs_zrev_read(&r, lle.nbits);
				ml_state = mle.base + btrfs_zrev_read(&r, mle.nbits);
				of_state = ofe.base + btrfs_zrev_read(&r, ofe.nbits);
				if (r.bits < 0) return false;
			}

			/* Offsets 1..3 pick a recent offset (shifted by one when there are no literals). */
			uint32_t offset;

			if (offset_value > 3U) {
				offset = offset_value - 3U;
				z->rep[2] = z->rep[1];
				z->rep[1] = z->rep[0];
				z->rep[0] = offset;
			} else {
				unsigned index = offset_value - 1U + (literal == 0U ? 1U : 0U);

				if (index == 0U) {
					offset = z->rep[0];
				} else if (index == 3U) {
					offset = z->rep[0] - 1U;
					if (offset == 0U) return false;
					z->rep[2] = z->rep[1];
					z->rep[1] = z->rep[0];
					z->rep[0] = offset;
				} else {
					offset = z->rep[index];
					if (index == 2U) z->rep[2] = z->rep[1];
					z->rep[1] = z->rep[0];
					z->rep[0] = offset;
				}
			}

			if (literal > lit_size - lit_pos || literal > (size_t)(out_end - out)) return false;
			memcpy(out, lits + lit_pos, literal);
			out += literal;
			lit_pos += literal;

			if (offset > (size_t)(out - frame_start) || match > (size_t)(out_end - out)) return false;
			for (uint32_t k = 0U; k < match; k++) {
				*out = *(out - offset);
				out++;
			}
		}

		/* The stream must end exactly where the last sequence's bits end. */
		if (r.bits != 0) return false;
	} else if (pos != nbytes) {
		return false;
	}

	/* Whatever literals are left follow the last match. */
	size_t tail = lit_size - lit_pos;

	if (tail > (size_t)(out_end - out)) return false;
	memcpy(out, lits + lit_pos, tail);
	out += tail;

	*op = out;
	return true;
}

/* ---- blocks and frames ---------------------------------------------------------------------------- */

static bool btrfs_zstd_compressed_block(btrfs_zstd_state_t *z, const uint8_t *in, size_t nbytes, uint8_t *frame_start, uint8_t **op, uint8_t *out_end)
{
	const uint8_t *lits;
	size_t lit_size;
	size_t used;

	if (!btrfs_zstd_literals(z, in, nbytes, (size_t)(out_end - *op), &lits, &lit_size, &used)) return false;

	return btrfs_zstd_sequences(z, in + used, nbytes - used, lits, lit_size, frame_start, op, out_end);
}

/*
 * One frame after its magic. *used is how many input bytes it took, *op the
 * output cursor (in and out). The frame's output may not exceed out_end.
 */
static bool btrfs_zstd_frame(btrfs_zstd_state_t *z, const uint8_t *in, size_t nbytes, size_t *used, uint8_t **op, uint8_t *out_end)
{
	size_t pos = 0U;
	uint8_t *frame_start = *op;

	if (nbytes < 1U) return false;

	unsigned descriptor = in[pos++];
	unsigned fcs_flag = descriptor >> 6U;
	bool single_segment = (descriptor & 0x20U) != 0U;
	bool checksum = (descriptor & 0x04U) != 0U;
	unsigned dict_flag = descriptor & 3U;

	if ((descriptor & 0x08U) != 0U) return false;

	uint64_t window = 0U;

	if (!single_segment) {
		if (nbytes - pos < 1U) return false;

		unsigned byte = in[pos++];
		unsigned exponent = byte >> 3U;
		unsigned mantissa = byte & 7U;

		if (exponent + 10U > 17U) return false;
		window = (1ULL << (10U + exponent));
		window += (window / 8U) * mantissa;
	}

	/* No dictionaries: a frame may name one only as id 0. */
	unsigned dict_bytes = dict_flag == 0U ? 0U : (dict_flag == 1U ? 1U : (dict_flag == 2U ? 2U : 4U));

	if (nbytes - pos < dict_bytes) return false;
	for (unsigned i = 0U; i < dict_bytes; i++) {
		if (in[pos + i] != 0U) return false;
	}
	pos += dict_bytes;

	unsigned fcs_bytes = fcs_flag == 0U ? (single_segment ? 1U : 0U) : (fcs_flag == 1U ? 2U : (fcs_flag == 2U ? 4U : 8U));
	bool have_size = fcs_bytes != 0U;
	uint64_t content_size = 0U;

	if (nbytes - pos < fcs_bytes) return false;
	for (unsigned i = 0U; i < fcs_bytes; i++) content_size |= (uint64_t)in[pos + i] << (8U * i);
	pos += fcs_bytes;
	if (fcs_flag == 1U) content_size += 256U;

	if (single_segment) window = content_size;
	if (window > BTRFS_MAX_COMPRESSED_EXTENT) return false;
	if (have_size && content_size > (uint64_t)(out_end - frame_start)) return false;

	uint64_t block_max = window < ZSTD_BLOCK_MAX ? window : ZSTD_BLOCK_MAX;

	z->rep[0] = 1U;
	z->rep[1] = 4U;
	z->rep[2] = 8U;
	z->ll.valid = false;
	z->of.valid = false;
	z->ml.valid = false;
	z->huf.valid = false;

	for (bool last = false; !last;) {
		if (nbytes - pos < 3U) return false;

		uint32_t header = (uint32_t)in[pos] | ((uint32_t)in[pos + 1U] << 8U) | ((uint32_t)in[pos + 2U] << 16U);
		unsigned type = (header >> 1U) & 3U;
		size_t size = header >> 3U;

		last = (header & 1U) != 0U;
		pos += 3U;

		if (size > block_max) return false;

		if (type == 0U) {
			if (size > nbytes - pos || size > (size_t)(out_end - *op)) return false;
			memcpy(*op, in + pos, size);
			*op += size;
			pos += size;
		} else if (type == 1U) {
			if (nbytes - pos < 1U || size > (size_t)(out_end - *op)) return false;
			memset(*op, in[pos], size);
			*op += size;
			pos += 1U;
		} else if (type == 2U) {
			if (size > nbytes - pos) return false;
			if (!btrfs_zstd_compressed_block(z, in + pos, size, frame_start, op, out_end)) return false;
			pos += size;
		} else {
			return false;
		}
	}

	size_t produced = (size_t)(*op - frame_start);

	if (have_size && content_size != produced) return false;

	if (checksum) {
		if (nbytes - pos < 4U) return false;

		uint32_t stored = (uint32_t)in[pos] | ((uint32_t)in[pos + 1U] << 8U) | ((uint32_t)in[pos + 2U] << 16U) | ((uint32_t)in[pos + 3U] << 24U);

		if (stored != (uint32_t)btrfs_xxh64(frame_start, produced, 0ULL)) return false;
		pos += 4U;
	}

	*used = pos;
	return true;
}

btrfs_status_t btrfs_zstd_decompress(void *ctx, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	const btrfs_codec_ctx_t *codec = ctx;
	size_t literals_cap = out_len < ZSTD_BLOCK_MAX ? out_len : ZSTD_BLOCK_MAX;

	if (out_len == 0U || out_len > BTRFS_MAX_COMPRESSED_EXTENT) return BTRFS_ERR_CORRUPT;

	btrfs_zstd_state_t *z = codec->env->alloc(codec->env->ctx, sizeof(*z) + literals_cap);
	if (z == 0) return BTRFS_ERR_NOMEM;

	z->literals = (uint8_t *)(z + 1);
	z->literals_cap = literals_cap;

	size_t pos = 0U;
	uint8_t *op = out;
	uint8_t *out_end = out + out_len;
	bool ok = true;

	/* Frames one after another until the output is complete; what follows is sector padding. */
	while (ok && pos < in_len && op < out_end) {
		/* No frame starts with a zero byte: what is left is the padding (checked below). */
		if (in[pos] == 0U) break;

		if (in_len - pos < 4U) {
			ok = false;
			break;
		}

		uint32_t magic = (uint32_t)in[pos] | ((uint32_t)in[pos + 1U] << 8U) | ((uint32_t)in[pos + 2U] << 16U) | ((uint32_t)in[pos + 3U] << 24U);

		pos += 4U;

		if ((magic & ZSTD_SKIPPABLE_MASK) == ZSTD_SKIPPABLE_MAGIC) {
			if (in_len - pos < 4U) {
				ok = false;
				break;
			}

			uint32_t skip = (uint32_t)in[pos] | ((uint32_t)in[pos + 1U] << 8U) | ((uint32_t)in[pos + 2U] << 16U) | ((uint32_t)in[pos + 3U] << 24U);

			pos += 4U;
			if (skip > in_len - pos) ok = false;
			else pos += skip;
			continue;
		}

		if (magic != ZSTD_MAGIC) {
			ok = false;
			break;
		}

		size_t used;

		ok = btrfs_zstd_frame(z, in + pos, in_len - pos, &used, &op, out_end);
		if (ok) pos += used;
	}

	/* The last extent of a file is rounded up to a sector; the rest reads as zeros. */
	if (ok) memset(op, 0, (size_t)(out_end - op));

	for (; ok && pos < in_len; pos++) {
		if (in[pos] != 0U) ok = false;
	}

	codec->env->release(codec->env->ctx, z);
	return ok ? BTRFS_OK : BTRFS_ERR_CORRUPT;
}
