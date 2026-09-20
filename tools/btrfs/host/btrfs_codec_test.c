/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        tools/btrfs/host/btrfs_codec_test.c
 *
 * Host test driver for the freestanding decoders (vfs/btrfs/btrfs_zlib.c,
 * btrfs_lzo.c, btrfs_zstd.c), reached as `btrfs_host codec ...`. Built with
 * ASan + UBSan by `make btrfs-host`; tools/btrfs/test_host.sh feeds it
 * streams made by real compressors (python's zlib, the zstd command) and by the
 * LZO generator below.
 *
 *   codec decode KIND STREAM ORIGINAL [--sector N] [--pad N] [--outlen N]
 *       decode STREAM into a buffer of exactly outlen (default: the size of
 *       ORIGINAL plus pad) and compare; prints "decode=<status> match=<yes|no>"
 *   codec fuzz KIND STREAM ORIGINAL [--seed N] [--iters N] [--sector N] [--integrity]
 *       mutate STREAM (bit flips, byte edits, truncation, zeroed runs, copied
 *       blocks, extended junk) and decode each into an exact-size buffer; only
 *       errors or the original bytes are acceptable when --integrity says the
 *       format detects damage (zlib's Adler-32, zstd's content checksum)
 *   codec lzo ORIGINAL OUT [--sector N] [--seed N]
 *       write ORIGINAL as a Btrfs LZO extent, made by a small LZO1X compressor
 *       that exercises every instruction form (a real compressor never would)
 *   codec xxh64
 *       xxh64 known-answer vectors
 *
 * KIND is zlib, lzo or zstd. Every decode runs with a counting allocator; a
 * leak is a failure.
 */

#include "../../../vfs/btrfs/btrfs_codec.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	uint64_t s;
} crng_t;

static uint64_t crng_next(crng_t *r)
{
	r->s ^= r->s << 13U;
	r->s ^= r->s >> 7U;
	r->s ^= r->s << 17U;
	return r->s;
}

static uint64_t crng_below(crng_t *r, uint64_t n)
{
	return n == 0U ? 0U : crng_next(r) % n;
}

static long g_allocs;

static void *codec_alloc(void *ctx, size_t size)
{
	(void)ctx;
	g_allocs++;
	return calloc(1U, size != 0U ? size : 1U);
}

static void codec_release(void *ctx, void *pointer)
{
	(void)ctx;
	if (pointer != NULL) g_allocs--;
	free(pointer);
}

static uint8_t *slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;
	uint8_t *data;

	if (f == NULL) {
		fprintf(stderr, "codec: %s: %s\n", path, strerror(errno));
		exit(2);
	}

	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(n > 0 ? (size_t)n : 1U);
	if (n > 0 && fread(data, 1U, (size_t)n, f) != (size_t)n) exit(2);
	fclose(f);
	*size = (size_t)n;
	return data;
}

static btrfs_status_t codec_run(const char *kind, uint32_t sector, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	btrfs_env_t env = { NULL, codec_alloc, codec_release, NULL };
	btrfs_codec_ctx_t ctx = { &env, sector };

	if (strcmp(kind, "zlib") == 0) return btrfs_zlib_decompress(&ctx, in, in_len, out, out_len);
	if (strcmp(kind, "lzo") == 0) return btrfs_lzo_decompress(&ctx, in, in_len, out, out_len);
	if (strcmp(kind, "zstd") == 0) return btrfs_zstd_decompress(&ctx, in, in_len, out, out_len);

	fprintf(stderr, "codec: unknown kind %s\n", kind);
	exit(2);
}

static uint64_t opt_u64(int argc, char **argv, const char *name, uint64_t fallback)
{
	for (int i = 0; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) return strtoull(argv[i + 1], NULL, 0);
	}

	return fallback;
}

static bool opt_flag(int argc, char **argv, const char *name)
{
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], name) == 0) return true;
	}

	return false;
}

/* ---- decode ------------------------------------------------------------------------------------- */

static int cmd_decode(int argc, char **argv)
{
	size_t in_len;
	size_t orig_len;
	uint8_t *in = slurp(argv[1], &in_len);
	uint8_t *orig = slurp(argv[2], &orig_len);
	uint32_t sector = (uint32_t)opt_u64(argc, argv, "--sector", 4096U);
	size_t pad = (size_t)opt_u64(argc, argv, "--pad", 0U);
	size_t out_len = (size_t)opt_u64(argc, argv, "--outlen", orig_len + pad);
	uint8_t *out = malloc(out_len != 0U ? out_len : 1U);
	long before = g_allocs;

	memset(out, 0xA5, out_len);

	btrfs_status_t status = codec_run(argv[0], sector, in, in_len, out, out_len);
	bool match = false;

	if (status == BTRFS_OK) {
		match = out_len >= orig_len && memcmp(out, orig, orig_len) == 0;
		for (size_t i = orig_len; match && i < out_len; i++) match = out[i] == 0U;
	}

	printf("decode=%s match=%s leaked=%ld\n", btrfs_status_name(status), match ? "yes" : "no", g_allocs - before);
	free(in);
	free(orig);
	free(out);
	return status == BTRFS_OK && match && g_allocs == before ? 0 : 1;
}

/* ---- fuzz ------------------------------------------------------------------------------------- */

static void alarm_handler(int signal_number)
{
	(void)signal_number;
	static const char message[] = "codec: fuzz iteration hung (alarm)\n";
	(void)!write(2, message, sizeof(message) - 1U);
	_exit(3);
}

static size_t mutate(crng_t *rng, const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
	size_t n = len;

	memcpy(dst, src, len);

	switch (crng_below(rng, 8U)) {
	case 0:
	case 1:
		for (uint64_t k = 1U + crng_below(rng, 8U); k != 0U && n != 0U; k--) dst[crng_below(rng, n)] ^= (uint8_t)(1U << crng_below(rng, 8U));
		break;
	case 2:
		if (n != 0U) dst[crng_below(rng, n)] = (uint8_t)crng_below(rng, 256U);
		break;
	case 3:
		if (n != 0U) dst[crng_below(rng, n)] = crng_below(rng, 2U) != 0U ? 0xFFU : 0U;
		break;
	case 4:
		n = crng_below(rng, len + 1U);
		break;
	case 5:
		if (n > 8U) {
			size_t at = crng_below(rng, n - 4U);
			size_t run = 1U + crng_below(rng, n - at > 64U ? 64U : n - at);
			memset(dst + at, crng_below(rng, 2U) != 0U ? 0U : 0xFFU, run);
		}
		break;
	case 6:
		if (n > 16U) {
			size_t from = crng_below(rng, n - 8U);
			size_t to = crng_below(rng, n - 8U);
			memmove(dst + to, dst + from, 1U + crng_below(rng, 8U));
		}
		break;
	default:
		for (uint64_t k = 1U + crng_below(rng, 8U); k != 0U && n < cap; k--) dst[n++] = (uint8_t)crng_below(rng, 256U);
		break;
	}

	return n;
}

static int cmd_fuzz(int argc, char **argv)
{
	size_t in_len;
	size_t orig_len;
	uint8_t *in = slurp(argv[1], &in_len);
	uint8_t *orig = slurp(argv[2], &orig_len);
	uint32_t sector = (uint32_t)opt_u64(argc, argv, "--sector", 4096U);
	uint64_t iters = opt_u64(argc, argv, "--iters", 2000U);
	bool integrity = opt_flag(argc, argv, "--integrity");
	crng_t rng = { opt_u64(argc, argv, "--seed", 1U) * 0x9E3779B97F4A7C15ULL + 1U };
	size_t cap = in_len + 64U;
	uint8_t *mutant = malloc(cap);
	uint64_t ok = 0U;
	uint64_t refused = 0U;
	uint64_t wrong = 0U;

	signal(SIGALRM, alarm_handler);

	for (uint64_t i = 0U; i < iters; i++) {
		/* A fresh exact-size allocation each time so ASan sees any write past the end. */
		size_t out_len = orig_len;
		uint8_t *out = malloc(out_len != 0U ? out_len : 1U);
		size_t n = mutate(&rng, in, in_len, mutant, cap);
		uint8_t *exact = malloc(n != 0U ? n : 1U);
		long before = g_allocs;

		memcpy(exact, mutant, n);
		alarm(20);
		btrfs_status_t status = codec_run(argv[0], sector, exact, n, out, out_len);
		alarm(0);

		if (g_allocs != before) {
			fprintf(stderr, "codec: iteration %llu leaked %ld allocations\n", (unsigned long long)i, g_allocs - before);
			return 1;
		}

		if (status == BTRFS_OK) {
			ok++;
			if (integrity && memcmp(out, orig, orig_len) != 0) {
				wrong++;
				fprintf(stderr, "codec: iteration %llu: damaged stream decoded to different data\n", (unsigned long long)i);
			}
		} else {
			refused++;
		}

		free(exact);
		free(out);
	}

	printf("fuzz=%s iterations=%llu decoded=%llu refused=%llu wrong=%llu\n", argv[0], (unsigned long long)iters, (unsigned long long)ok, (unsigned long long)refused, (unsigned long long)wrong);
	free(mutant);
	free(in);
	free(orig);
	return wrong == 0U ? 0 : 1;
}

/* ---- an LZO1X compressor that uses every instruction form -------------------------------------- */

typedef struct {
	uint8_t *out;
	size_t n;
	unsigned state;     /* the decoder's view: 0..3 trailing literals, 4 after a literal run */
} lzo_w_t;

static void lzo_put(lzo_w_t *w, unsigned byte)
{
	w->out[w->n++] = (uint8_t)byte;
}

/* Extension bytes for a length whose base is already in the instruction: v >= 1 as 255*zeros + last. */
static void lzo_ext(lzo_w_t *w, size_t v)
{
	size_t zeros = (v - 1U) / 255U;

	while (zeros-- != 0U) lzo_put(w, 0U);
	lzo_put(w, (unsigned)(v - 255U * ((v - 1U) / 255U)));
}

/* A literal run instruction (state 0 only): 4 or more literals. */
static void lzo_literal_run(lzo_w_t *w, const uint8_t *lit, size_t n)
{
	if (n <= 18U) {
		lzo_put(w, (unsigned)(n - 3U));
	} else {
		lzo_put(w, 0U);
		lzo_ext(w, n - 18U);
	}

	for (size_t i = 0U; i < n; i++) lzo_put(w, lit[i]);
	w->state = 4U;
}

/* dist >= 1, len >= 2. `trail` literals (0..3) follow and are emitted by the caller. */
static bool lzo_match(lzo_w_t *w, size_t dist, size_t len, unsigned trail, crng_t *rng)
{
	if (len == 2U && w->state >= 1U && w->state <= 3U && dist <= 1024U) {
		lzo_put(w, (unsigned)((((dist - 1U) & 3U) << 2U) | trail));
		lzo_put(w, (unsigned)((dist - 1U) >> 2U));
	} else if (len == 3U && w->state == 4U && dist >= 2049U && dist <= 3072U) {
		size_t d = dist - 1U - 0x800U;

		lzo_put(w, (unsigned)(((d & 3U) << 2U) | trail));
		lzo_put(w, (unsigned)(d >> 2U));
	} else if (len >= 3U && len <= 8U && dist <= 2048U && crng_below(rng, 4U) != 0U) {
		lzo_put(w, (unsigned)(((len - 1U) << 5U) | (((dist - 1U) & 7U) << 2U) | trail));
		lzo_put(w, (unsigned)((dist - 1U) >> 3U));
	} else if (len >= 3U && dist <= 16384U) {
		size_t t = len - 2U;

		if (t <= 31U) {
			lzo_put(w, (unsigned)(32U | t));
		} else {
			lzo_put(w, 32U);
			lzo_ext(w, t - 31U);
		}

		lzo_put(w, (unsigned)((((dist - 1U) & 63U) << 2U) | trail));
		lzo_put(w, (unsigned)((dist - 1U) >> 6U));
	} else if (len >= 3U && dist >= 0x4001U && dist <= 0xBFFFU) {
		size_t t = len - 2U;
		unsigned high = dist >= 0x8000U ? 8U : 0U;
		size_t low = dist - 0x4000U - (high != 0U ? 0x4000U : 0U);

		if (t <= 7U) {
			lzo_put(w, (unsigned)(16U | high | t));
		} else {
			lzo_put(w, 16U | high);
			lzo_ext(w, t - 7U);
		}

		lzo_put(w, (unsigned)(((low & 63U) << 2U) | trail));
		lzo_put(w, (unsigned)(low >> 6U));
	} else {
		return false;
	}

	w->state = trail;
	return true;
}

typedef struct {
	size_t lits;     /* literals since the previous match */
	size_t len;
	size_t dist;
} lzo_token_t;

/* The first instruction of a stream: n >= 1 literals. */
static void lzo_first_literals(lzo_w_t *w, const uint8_t *lit, size_t n, crng_t *rng)
{
	if (n <= 3U || (n <= 238U && crng_below(rng, 2U) == 0U)) {
		lzo_put(w, (unsigned)(n + 17U));
		for (size_t i = 0U; i < n; i++) lzo_put(w, lit[i]);
		w->state = n < 4U ? (unsigned)n : 4U;
	} else {
		lzo_literal_run(w, lit, n);
	}
}

/* One segment: an LZO1X stream for src[0..n), independent of any other. dst needs n + n/8 + 64 bytes. */
static size_t lzo_compress(const uint8_t *src, size_t n, uint8_t *dst, crng_t *rng)
{
	lzo_w_t w = { dst, 0U, 0U };
	size_t *head = malloc(4096U * sizeof(size_t));
	lzo_token_t *tokens = malloc((n / 2U + 2U) * sizeof(*tokens));
	size_t count = 0U;
	size_t i = 0U;
	size_t lit_start = 0U;

	for (size_t h = 0U; h < 4096U; h++) head[h] = (size_t)-1;

	while (i < n) {
		size_t best_len = 0U;
		size_t best_dist = 0U;

		if (i + 3U <= n) {
			uint32_t key = ((uint32_t)src[i] | ((uint32_t)src[i + 1U] << 8U) | ((uint32_t)src[i + 2U] << 16U)) * 2654435761U;
			size_t cand = head[key >> 20U];

			head[key >> 20U] = i;
			if (cand != (size_t)-1 && i - cand <= 0xBFFFU) {
				size_t len = 0U;

				while (i + len < n && src[cand + len] == src[i + len] && len < 300U) len++;

				if (len >= 3U) {
					best_len = len;
					best_dist = i - cand;
				}
			}
		}

		/* A two-byte match is only encodable right after 1..3 literals (state 1..3) and close by. */
		if (best_len == 0U && count != 0U && i - lit_start >= 1U && i - lit_start <= 3U && i >= 1U && i + 2U <= n) {
			for (size_t d = 1U; d <= 1024U && d <= i; d++) {
				if (src[i - d] == src[i] && src[i - d + 1U] == src[i + 1U]) {
					best_len = 2U;
					best_dist = d;
					break;
				}
			}
		}

		/* Sometimes take a shorter match than the longest, to vary the lengths. */
		if (best_len > 8U && crng_below(rng, 5U) == 0U) best_len = 3U + crng_below(rng, best_len - 3U);

		if (best_len == 0U) {
			i++;
			continue;
		}

		tokens[count].lits = i - lit_start;
		tokens[count].len = best_len;
		tokens[count].dist = best_dist;
		count++;
		i += best_len;
		lit_start = i;
	}

	size_t final_lits = n - lit_start;
	size_t cursor = 0U;

	if (count == 0U) {
		lzo_first_literals(&w, src, n, rng);
	} else {
		lzo_first_literals(&w, src, tokens[0].lits, rng);
		cursor = tokens[0].lits;

		for (size_t t = 0U; t < count; t++) {
			size_t next = t + 1U < count ? tokens[t + 1U].lits : final_lits;
			unsigned trail = next <= 3U ? (unsigned)next : 0U;

			/* One form is picked by state, length and distance; M1 far is taken half the time it fits. */
			if (w.state == 4U && tokens[t].len == 3U && tokens[t].dist >= 2049U && tokens[t].dist <= 3072U && crng_below(rng, 2U) == 0U) {
				size_t d = tokens[t].dist - 1U - 0x800U;

				lzo_put(&w, (unsigned)(((d & 3U) << 2U) | trail));
				lzo_put(&w, (unsigned)(d >> 2U));
				w.state = trail;
			} else if (!lzo_match(&w, tokens[t].dist, tokens[t].len, trail, rng)) {
				fprintf(stderr, "codec: lzo generator produced an unencodable match (len %zu dist %zu)\n", tokens[t].len, tokens[t].dist);
				exit(2);
			}

			cursor += tokens[t].len;

			if (next <= 3U) {
				for (size_t k = 0U; k < next; k++) lzo_put(&w, src[cursor + k]);
			} else {
				lzo_literal_run(&w, src + cursor, next);
			}

			cursor += next;
		}
	}

	/* End of stream. */
	lzo_put(&w, 17U);
	lzo_put(&w, 0U);
	lzo_put(&w, 0U);

	free(head);
	free(tokens);
	return w.n;
}

/* A Btrfs LZO extent: total length, then per sector a length-prefixed segment; headers never straddle a sector. */
static int cmd_lzo(int argc, char **argv)
{
	size_t n;
	uint8_t *src = slurp(argv[0], &n);
	size_t sector = (size_t)opt_u64(argc, argv, "--sector", 4096U);
	crng_t rng = { opt_u64(argc, argv, "--seed", 1U) * 0x9E3779B97F4A7C15ULL + 1U };
	size_t cap = 4U + (n / sector + 2U) * (sector + sector / 8U + 64U + 4U + sector);
	uint8_t *dst = calloc(1U, cap);
	uint8_t *seg = malloc(sector + sector / 4U + 128U);
	size_t pos = 4U;

	for (size_t off = 0U; off < n; off += sector) {
		size_t piece = n - off < sector ? n - off : sector;
		size_t left = sector - (pos & (sector - 1U));
		size_t used;

		if (left < 4U) pos += left;

		used = lzo_compress(src + off, piece, seg, &rng);
		dst[pos] = (uint8_t)used;
		dst[pos + 1U] = (uint8_t)(used >> 8U);
		dst[pos + 2U] = (uint8_t)(used >> 16U);
		dst[pos + 3U] = (uint8_t)(used >> 24U);
		memcpy(dst + pos + 4U, seg, used);
		pos += 4U + used;
	}

	dst[0] = (uint8_t)pos;
	dst[1] = (uint8_t)(pos >> 8U);
	dst[2] = (uint8_t)(pos >> 16U);
	dst[3] = (uint8_t)(pos >> 24U);

	/* The extent is stored padded to whole sectors. */
	size_t stored = (pos + sector - 1U) / sector * sector;
	FILE *f = fopen(argv[1], "wb");

	if (f == NULL || fwrite(dst, 1U, stored, f) != stored) exit(2);
	fclose(f);
	free(dst);
	free(seg);
	free(src);
	return 0;
}

/* ---- known-answer vectors --------------------------------------------------------------------- */

static int cmd_hash(void)
{
	static const struct {
		const char *text;
		uint64_t seed;
		uint64_t want;
	} vectors[] = {
		{ "", 0U, 0xEF46DB3751D8E999ULL },
		{ "a", 0U, 0xD24EC4F1A98C6E5BULL },
		{ "abc", 0U, 0x44BC2CF5AD770999ULL },
	};
	int bad = 0;

	for (size_t i = 0U; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		uint64_t got = btrfs_xxh64(vectors[i].text, strlen(vectors[i].text), vectors[i].seed);

		if (got != vectors[i].want) {
			printf("hash: xxh64(\"%s\") = %016llx, want %016llx\n", vectors[i].text, (unsigned long long)got, (unsigned long long)vectors[i].want);
			bad++;
		}
	}

	/* The low 32 bits are what a ZSTD frame stores as its content checksum: from `zstd -lv` of this text. */
	static const char text[] = "hello hello hello hello world";

	if ((uint32_t)btrfs_xxh64(text, sizeof(text) - 1U, 0U) != 0xd4935fd7U) {
		printf("hash: low 32 bits of xxh64 of the zstd sample differ\n");
		bad++;
	}

	printf("xxh64=%s\n", bad == 0 ? "ok" : "bad");
	return bad == 0 ? 0 : 1;
}

int codec_main(int argc, char **argv)
{
	if (argc >= 4 && strcmp(argv[0], "decode") == 0) return cmd_decode(argc - 1, argv + 1);
	if (argc >= 4 && strcmp(argv[0], "fuzz") == 0) return cmd_fuzz(argc - 1, argv + 1);
	if (argc >= 3 && strcmp(argv[0], "lzo") == 0) return cmd_lzo(argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "xxh64") == 0) return cmd_hash();

	fprintf(stderr, "usage: btrfs_host codec decode|fuzz|lzo|xxh64 ...\n");
	return 2;
}
