/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_hash.c
 *
 * SHA-256 (FIPS 180-4) and BLAKE2b-256 (RFC 7693, unkeyed, 32-byte digest): the
 * two 32-byte checksum types of Btrfs (mkfs.btrfs --csum sha256 / blake2).
 * One-shot, no allocation, no dependencies; checked against the reference
 * digests by tools/btrfs/test_codec.sh (`btrfs_host codec digests`). Speed is
 * not the point: a 4 KiB sector costs a few microseconds.
 */

#include "btrfs_format.h"

#include <string.h>

/* ---- SHA-256 ---------------------------------------------------------------------------------- */

static const uint32_t g_sha256_k[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t btrfs_ror32(uint32_t value, unsigned bits)
{
	return (value >> bits) | (value << (32U - bits));
}

static void btrfs_sha256_block(uint32_t state[8], const uint8_t *block)
{
	uint32_t w[64];
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

	for (unsigned i = 0U; i < 16U; i++) {
		w[i] = ((uint32_t)block[i * 4U] << 24U) | ((uint32_t)block[i * 4U + 1U] << 16U) | ((uint32_t)block[i * 4U + 2U] << 8U) | block[i * 4U + 3U];
	}

	for (unsigned i = 16U; i < 64U; i++) {
		uint32_t s0 = btrfs_ror32(w[i - 15U], 7U) ^ btrfs_ror32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
		uint32_t s1 = btrfs_ror32(w[i - 2U], 17U) ^ btrfs_ror32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);

		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}

	for (unsigned i = 0U; i < 64U; i++) {
		uint32_t s1 = btrfs_ror32(e, 6U) ^ btrfs_ror32(e, 11U) ^ btrfs_ror32(e, 25U);
		uint32_t choose = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + choose + g_sha256_k[i] + w[i];
		uint32_t s0 = btrfs_ror32(a, 2U) ^ btrfs_ror32(a, 13U) ^ btrfs_ror32(a, 22U);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

void btrfs_sha256(const void *data, size_t length, uint8_t out[32])
{
	uint32_t state[8] = { 0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U };
	const uint8_t *p = data;
	size_t left = length;
	uint8_t tail[128];
	size_t tail_len;

	while (left >= 64U) {
		btrfs_sha256_block(state, p);
		p += 64;
		left -= 64U;
	}

	/* The rest, a 1 bit, zeros, and the length in bits as 64 big-endian bits. */
	memcpy(tail, p, left);
	tail[left] = 0x80U;
	tail_len = left < 56U ? 64U : 128U;
	memset(tail + left + 1U, 0, tail_len - left - 1U);

	uint64_t bits = (uint64_t)length * 8U;

	for (unsigned i = 0U; i < 8U; i++) tail[tail_len - 1U - i] = (uint8_t)(bits >> (8U * i));

	btrfs_sha256_block(state, tail);
	if (tail_len == 128U) btrfs_sha256_block(state, tail + 64);

	for (unsigned i = 0U; i < 8U; i++) {
		out[i * 4U] = (uint8_t)(state[i] >> 24U);
		out[i * 4U + 1U] = (uint8_t)(state[i] >> 16U);
		out[i * 4U + 2U] = (uint8_t)(state[i] >> 8U);
		out[i * 4U + 3U] = (uint8_t)state[i];
	}
}

/* ---- BLAKE2b ---------------------------------------------------------------------------------- */

static const uint64_t g_blake2b_iv[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const uint8_t g_blake2b_sigma[12][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 }
};

static uint64_t btrfs_ror64(uint64_t value, unsigned bits)
{
	return (value >> bits) | (value << (64U - bits));
}

#define BLAKE2B_G(v, a, b, c, d, x, y) \
	do { \
		v[a] = v[a] + v[b] + (x); \
		v[d] = btrfs_ror64(v[d] ^ v[a], 32U); \
		v[c] = v[c] + v[d]; \
		v[b] = btrfs_ror64(v[b] ^ v[c], 24U); \
		v[a] = v[a] + v[b] + (y); \
		v[d] = btrfs_ror64(v[d] ^ v[a], 16U); \
		v[c] = v[c] + v[d]; \
		v[b] = btrfs_ror64(v[b] ^ v[c], 63U); \
	} while (0)

static void btrfs_blake2b_compress(uint64_t h[8], const uint8_t block[128], uint64_t counter, bool last)
{
	uint64_t m[16];
	uint64_t v[16];

	for (unsigned i = 0U; i < 16U; i++) m[i] = btrfs_get_le64(block + i * 8U);
	for (unsigned i = 0U; i < 8U; i++) {
		v[i] = h[i];
		v[i + 8U] = g_blake2b_iv[i];
	}

	v[12] ^= counter;
	if (last) v[14] = ~v[14];

	for (unsigned round = 0U; round < 12U; round++) {
		const uint8_t *s = g_blake2b_sigma[round];

		BLAKE2B_G(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
		BLAKE2B_G(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
		BLAKE2B_G(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
		BLAKE2B_G(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
		BLAKE2B_G(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
		BLAKE2B_G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
		BLAKE2B_G(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
		BLAKE2B_G(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
	}

	for (unsigned i = 0U; i < 8U; i++) h[i] ^= v[i] ^ v[i + 8U];
}

void btrfs_blake2b_256(const void *data, size_t length, uint8_t out[32])
{
	uint64_t h[8];
	const uint8_t *p = data;
	size_t left = length;
	uint64_t counter = 0U;
	uint8_t last[128];

	for (unsigned i = 0U; i < 8U; i++) h[i] = g_blake2b_iv[i];

	/* Parameter block: digest length 32, no key, fanout 1, depth 1. */
	h[0] ^= 0x01010000ULL ^ 32ULL;

	/* The last block (even a full one, and the empty message) carries the final flag. */
	while (left > 128U) {
		counter += 128U;
		btrfs_blake2b_compress(h, p, counter, false);
		p += 128;
		left -= 128U;
	}

	memset(last, 0, sizeof(last));
	memcpy(last, p, left);
	counter += left;
	btrfs_blake2b_compress(h, last, counter, true);

	for (unsigned i = 0U; i < 32U; i++) out[i] = (uint8_t)(h[i / 8U] >> (8U * (i % 8U)));
}
