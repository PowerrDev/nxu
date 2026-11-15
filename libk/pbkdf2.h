/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        libk/pbkdf2.h
 *
 * HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 8018) over libk's
 * SHA-256, for the kernel's own password records: the login passcode
 * (kern/security/passcode.c) and the account the setup assistant writes
 * into /etc/shadow. The record format is the one frameworks/lib/auth.c
 * reads, "pbkdf2_sha256$<iterations>$<salt-hex>$<hash-hex>".
 */

#ifndef NXU_LIBK_PBKDF2_H
#define NXU_LIBK_PBKDF2_H

#include <libk/sha256.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HMAC-SHA256 of the message first || second (second may be empty). */
void hmac_sha256(const void *key, size_t key_length, const void *first, size_t first_length, const void *second,
				 size_t second_length, uint8_t out[SHA256_DIGEST_SIZE]);

/* PBKDF2-HMAC-SHA256 with any output length; iterations is at least 1. */
void pbkdf2_hmac_sha256(const void *password, size_t password_length, const void *salt, size_t salt_length,
						uint32_t iterations, uint8_t *out, size_t out_length);

/* Compare two buffers in time that depends only on their length. */
bool crypto_equal(const void *a, const void *b, size_t length);

/* Overwrite memory the compiler may not drop as a dead store. */
void crypto_wipe(void *memory, size_t size);

#endif
