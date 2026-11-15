/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        libk/pbkdf2.c
 *
 * See libk/pbkdf2.h. The pads are hashed once per key, not once per HMAC,
 * so each PBKDF2 iteration costs two SHA-256 compressions.
 */

#include <libk/pbkdf2.h>

#include <libk/string.h>

void
crypto_wipe(void *memory, size_t size)
{
	volatile uint8_t *bytes = (volatile uint8_t *)memory;

	while (size-- != 0U)
		*bytes++ = 0U;
}

bool
crypto_equal(const void *a, const void *b, size_t length)
{
	const volatile uint8_t *left = (const volatile uint8_t *)a;
	const volatile uint8_t *right = (const volatile uint8_t *)b;
	uint8_t difference = 0U;

	for (size_t index = 0U; index < length; index++)
		difference |= (uint8_t)(left[index] ^ right[index]);
	return difference == 0U;
}

/* The inner and outer contexts with the key's pads already hashed. */
typedef struct {
	sha256_context_t inner;
	sha256_context_t outer;
} hmac_sha256_key_t;

static void
hmac_sha256_key(hmac_sha256_key_t *state, const void *key, size_t key_length)
{
	uint8_t block[SHA256_BLOCK_SIZE] = {0};

	if (key_length > SHA256_BLOCK_SIZE)
		sha256(key, key_length, block);
	else if (key_length != 0U)
		memcpy(block, key, key_length);

	for (uint32_t index = 0U; index < SHA256_BLOCK_SIZE; index++)
		block[index] ^= 0x36U;
	sha256_init(&state->inner);
	sha256_update(&state->inner, block, SHA256_BLOCK_SIZE);

	for (uint32_t index = 0U; index < SHA256_BLOCK_SIZE; index++)
		block[index] ^= 0x36U ^ 0x5CU;
	sha256_init(&state->outer);
	sha256_update(&state->outer, block, SHA256_BLOCK_SIZE);
	crypto_wipe(block, sizeof(block));
}

static void
hmac_sha256_keyed(const hmac_sha256_key_t *state, const void *first, size_t first_length, const void *second,
				  size_t second_length, uint8_t out[SHA256_DIGEST_SIZE])
{
	sha256_context_t context = state->inner;
	uint8_t inner[SHA256_DIGEST_SIZE];

	if (first_length != 0U) sha256_update(&context, first, first_length);
	if (second_length != 0U) sha256_update(&context, second, second_length);
	sha256_final(&context, inner);

	context = state->outer;
	sha256_update(&context, inner, sizeof(inner));
	sha256_final(&context, out);
	crypto_wipe(inner, sizeof(inner));
	crypto_wipe(&context, sizeof(context));
}

void
hmac_sha256(const void *key, size_t key_length, const void *first, size_t first_length, const void *second,
			size_t second_length, uint8_t out[SHA256_DIGEST_SIZE])
{
	hmac_sha256_key_t state;

	hmac_sha256_key(&state, key, key_length);
	hmac_sha256_keyed(&state, first, first_length, second, second_length, out);
	crypto_wipe(&state, sizeof(state));
}

void
pbkdf2_hmac_sha256(const void *password, size_t password_length, const void *salt, size_t salt_length,
				   uint32_t iterations, uint8_t *out, size_t out_length)
{
	hmac_sha256_key_t state;
	uint8_t u[SHA256_DIGEST_SIZE], t[SHA256_DIGEST_SIZE];

	if (iterations == 0U) iterations = 1U;
	hmac_sha256_key(&state, password, password_length);

	for (uint32_t block = 1U; out_length != 0U; block++) {
		uint8_t counter[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block};
		size_t take = out_length < SHA256_DIGEST_SIZE ? out_length : SHA256_DIGEST_SIZE;

		hmac_sha256_keyed(&state, salt, salt_length, counter, sizeof(counter), u);
		memcpy(t, u, sizeof(t));
		for (uint32_t iteration = 1U; iteration < iterations; iteration++) {
			hmac_sha256_keyed(&state, u, sizeof(u), 0, 0U, u);
			for (uint32_t index = 0U; index < SHA256_DIGEST_SIZE; index++)
				t[index] ^= u[index];
		}
		memcpy(out, t, take);
		out += take;
		out_length -= take;
	}
	crypto_wipe(u, sizeof(u));
	crypto_wipe(t, sizeof(t));
	crypto_wipe(&state, sizeof(state));
}
