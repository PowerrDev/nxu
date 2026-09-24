#ifndef NXU_SHA256_H
#define NXU_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32U
#define SHA256_BLOCK_SIZE 64U

/*
 * SHA-256 (FIPS 180-4), for measuring images. A hash, not a MAC: it says
 * what bytes were read, and is only as trustworthy as the code that reads
 * them.
 */
typedef struct {
	uint32_t state[8];
	uint64_t length;                  /* bytes hashed so far */
	uint8_t block[SHA256_BLOCK_SIZE];
	size_t used;                      /* bytes waiting in block */
} sha256_context_t;

void sha256_init(sha256_context_t *context);
void sha256_update(sha256_context_t *context, const void *data, size_t size);
void sha256_final(sha256_context_t *context, uint8_t digest[SHA256_DIGEST_SIZE]);

/* The digest of one buffer. */
void sha256(const void *data, size_t size, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif
