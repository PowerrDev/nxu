#include <sha256.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint32_t sha256_k[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t sha256_rotr(uint32_t value, uint32_t count)
{
	return (value >> count) | (value << (32U - count));
}

/* One 64-byte block into the state (FIPS 180-4 section 6.2.2). */
static void sha256_block(uint32_t state[8], const uint8_t *block)
{
	uint32_t w[64];

	for (uint32_t t = 0U; t < 16U; t++) {
		w[t] = (uint32_t)block[4U * t] << 24U | (uint32_t)block[4U * t + 1U] << 16U |
			(uint32_t)block[4U * t + 2U] << 8U | (uint32_t)block[4U * t + 3U];
	}
	for (uint32_t t = 16U; t < 64U; t++) {
		uint32_t s0 = sha256_rotr(w[t - 15U], 7U) ^ sha256_rotr(w[t - 15U], 18U) ^ (w[t - 15U] >> 3U);
		uint32_t s1 = sha256_rotr(w[t - 2U], 17U) ^ sha256_rotr(w[t - 2U], 19U) ^ (w[t - 2U] >> 10U);

		w[t] = w[t - 16U] + s0 + w[t - 7U] + s1;
	}

	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

	for (uint32_t t = 0U; t < 64U; t++) {
		uint32_t t1 = h + (sha256_rotr(e, 6U) ^ sha256_rotr(e, 11U) ^ sha256_rotr(e, 25U)) + ((e & f) ^ (~e & g)) + sha256_k[t] + w[t];
		uint32_t t2 = (sha256_rotr(a, 2U) ^ sha256_rotr(a, 13U) ^ sha256_rotr(a, 22U)) + ((a & b) ^ (a & c) ^ (b & c));

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

void sha256_init(sha256_context_t *context)
{
	static const uint32_t initial[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
	};

	memcpy(context->state, initial, sizeof(initial));
	context->length = 0ULL;
	context->used = 0U;
}

void sha256_update(sha256_context_t *context, const void *data, size_t size)
{
	const uint8_t *bytes = data;

	context->length += size;

	while (size != 0U) {
		if (context->used == 0U && size >= SHA256_BLOCK_SIZE) {
			sha256_block(context->state, bytes);
			bytes += SHA256_BLOCK_SIZE;
			size -= SHA256_BLOCK_SIZE;
			continue;
		}

		size_t take = SHA256_BLOCK_SIZE - context->used;

		if (take > size) take = size;
		memcpy(context->block + context->used, bytes, take);
		context->used += take;
		bytes += take;
		size -= take;

		if (context->used == SHA256_BLOCK_SIZE) {
			sha256_block(context->state, context->block);
			context->used = 0U;
		}
	}
}

void sha256_final(sha256_context_t *context, uint8_t digest[SHA256_DIGEST_SIZE])
{
	uint64_t bits = context->length * 8ULL;

	context->block[context->used++] = 0x80U;
	if (context->used > SHA256_BLOCK_SIZE - 8U) {
		memset(context->block + context->used, 0, SHA256_BLOCK_SIZE - context->used);
		sha256_block(context->state, context->block);
		context->used = 0U;
	}
	memset(context->block + context->used, 0, SHA256_BLOCK_SIZE - 8U - context->used);
	for (uint32_t index = 0U; index < 8U; index++) context->block[SHA256_BLOCK_SIZE - 1U - index] = (uint8_t)(bits >> (8U * index));
	sha256_block(context->state, context->block);

	for (uint32_t index = 0U; index < 8U; index++) {
		digest[4U * index] = (uint8_t)(context->state[index] >> 24U);
		digest[4U * index + 1U] = (uint8_t)(context->state[index] >> 16U);
		digest[4U * index + 2U] = (uint8_t)(context->state[index] >> 8U);
		digest[4U * index + 3U] = (uint8_t)context->state[index];
	}
	memset(context, 0, sizeof(*context));
}

void sha256(const void *data, size_t size, uint8_t digest[SHA256_DIGEST_SIZE])
{
	sha256_context_t context;

	sha256_init(&context);
	sha256_update(&context, data, size);
	sha256_final(&context, digest);
}
