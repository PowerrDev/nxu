/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_crypto.h
 *
 * NXU's interface to the Trusted Enclave Processor's cryptographic services,
 * over the mailbox (drivers/tep/tep_mailbox.h, TEP_MB_FEATURE_CRYPTO).
 *
 * Keys are generated inside tepOS and named by an opaque 32-bit handle; the
 * private key never reaches NXU, only the public key and signatures do.
 * Every call fails closed: anything but TEP_OK means no answer from tepOS,
 * and nothing in NXU stands in for one.
 */

#ifndef NXU_DRIVERS_TEP_TEP_CRYPTO_H
#define NXU_DRIVERS_TEP_TEP_CRYPTO_H

#include <stdint.h>

#define TEP_SHA256_SIZE 32U
#define TEP_PUBLIC_KEY_SIZE 32U
#define TEP_SIGNATURE_SIZE 64U

typedef enum {
	TEP_OK = 0,
	TEP_ERR_UNAVAILABLE,    /* tepOS or the service is not available */
	TEP_ERR_INVALID,        /* bad arguments (sizes out of range) */
	TEP_ERR_NOT_FOUND,      /* no such key handle */
	TEP_ERR_FULL,           /* tepOS has no room for another key */
	TEP_ERR_PROTOCOL        /* tepOS answered something unexpected */
} tep_result_t;

tep_result_t tep_sha256(const void *data, uint32_t size, uint8_t digest[TEP_SHA256_SIZE]);

/* size is 1..TEP_MB_RANDOM_MAX (64). */
tep_result_t tep_random(void *out, uint32_t size);

/* A new Ed25519 key inside tepOS. */
tep_result_t tep_key_generate(uint32_t *handle);
tep_result_t tep_key_public(uint32_t handle, uint8_t public_key[TEP_PUBLIC_KEY_SIZE]);

/* size is 1..TEP_MB_SIGN_MAX (224). */
tep_result_t tep_key_sign(uint32_t handle, const void *message, uint32_t size, uint8_t signature[TEP_SIGNATURE_SIZE]);
tep_result_t tep_key_delete(uint32_t handle);

const char *tep_result_name(tep_result_t result);

#endif
