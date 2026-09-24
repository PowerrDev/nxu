/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_crypto.h
 *
 * NXU's interface to the Trusted Enclave Processor's services over the
 * mailbox (drivers/tep/tep_mailbox.h): cryptography (TEP_MB_FEATURE_CRYPTO),
 * the passcode (TEP_MB_FEATURE_AUTH) and boot manifests (TEP_MB_FEATURE_BOOT).
 *
 * Keys are generated inside tepOS and named by an opaque 32-bit handle; the
 * private key never reaches NXU, only the public key and signatures do.
 * The passcode is checked, delayed and locked out inside tepOS; NXU keeps no
 * copy of it or of its hash.
 *
 * Every call fails closed: anything but TEP_OK means tepOS did not say yes,
 * and nothing in NXU stands in for it.
 */

#ifndef NXU_DRIVERS_TEP_TEP_CRYPTO_H
#define NXU_DRIVERS_TEP_TEP_CRYPTO_H

#include <drivers/tep/tep_mailbox_proto.h>

#include <stdbool.h>
#include <stdint.h>

#define TEP_SHA256_SIZE 32U
#define TEP_PUBLIC_KEY_SIZE 32U
#define TEP_SIGNATURE_SIZE 64U

typedef enum {
	TEP_OK = 0,
	TEP_ERR_UNAVAILABLE,    /* tepOS or the service is not available */
	TEP_ERR_INVALID,        /* bad arguments (sizes out of range) */
	TEP_ERR_NOT_FOUND,      /* no such key handle, or no passcode set */
	TEP_ERR_FULL,           /* tepOS has no room for another key */
	TEP_ERR_PROTOCOL,       /* tepOS answered something unexpected */
	TEP_ERR_DENIED,         /* wrong passcode, or a boot manifest refused */
	TEP_ERR_RETRY_LATER,    /* too soon after failed passcodes */
	TEP_ERR_LOCKED,         /* passcode locked until a recovery reset in tepOS */
	TEP_ERR_ROLLBACK        /* boot image older than the newest accepted */
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

typedef struct {
	bool passcode_set;
	bool locked;
	uint8_t failures;       /* failed attempts since the last success */
	uint32_t wait_seconds;  /* before tepOS accepts another attempt */
} tep_auth_status_t;

/*
 * Passcodes are TEP_MB_PASSCODE_MIN..TEP_MB_PASSCODE_MAX (4..64) bytes.
 * TEP_ERR_NOT_FOUND from tep_auth_verify means no passcode is set;
 * TEP_ERR_RETRY_LATER sets *wait_seconds (0 for every other result).
 * tep_auth_set takes old_size 0 when no passcode is set yet.
 *
 * The passcode crosses the mailbox serial link as it is: the link is
 * checked for corruption, not encrypted, so the host can read it.
 */
tep_result_t tep_auth_set(const void *old_passcode, uint32_t old_size, const void *passcode, uint32_t size, uint32_t *wait_seconds);
tep_result_t tep_auth_verify(const void *passcode, uint32_t size, uint32_t *wait_seconds);
tep_result_t tep_auth_status(tep_auth_status_t *status);

/*
 * tep_boot_verify:
 *
 * Asks tepOS whether manifest (TEP_BOOT_MANIFEST_SIZE bytes, signed by the
 * host's boot-signing key) names the image whose SHA-256 NXU measured.
 * *detail is the image version on TEP_OK, the TEP_BOOT_* reason on
 * TEP_ERR_DENIED and the minimum version on TEP_ERR_ROLLBACK. The verdict
 * is only as good as NXU's own measurement, and nothing enforces it.
 */
tep_result_t tep_boot_verify(const uint8_t manifest[TEP_BOOT_MANIFEST_SIZE], const uint8_t digest[TEP_SHA256_SIZE], uint32_t *detail);

const char *tep_boot_reason_name(uint32_t reason);
const char *tep_result_name(tep_result_t result);

#endif
