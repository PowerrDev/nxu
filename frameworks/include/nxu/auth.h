/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * nxu/auth.h
 *
 * Password authentication against /etc/shadow. Passwords are stored as a crypt
 * string, never in the clear:
 *
 *     pbkdf2_sha256$<iterations>$<salt-hex>$<hash-hex>
 *
 * The hash is PBKDF2-HMAC-SHA256 of the password with the per-record salt. This
 * is an established, salted, iterated password KDF; Argon2id is the intended
 * upgrade and plugs in behind nxu_pwhash_make/nxu_pwverify without changing
 * callers.
 */
#ifndef NXU_AUTH_H
#define NXU_AUTH_H

#include <stdint.h>

#define NXU_SHADOW_PATH "/Volumes/mainOS/etc/shadow"

#define NXU_PW_SALT_LEN 16U
#define NXU_PW_HASH_LEN 32U
#define NXU_PW_ITERATIONS 4096U

/* Longest crypt string we produce or accept (algorithm + iters + 32-hex salt +
 * 64-hex hash + separators), with room to spare. */
#define NXU_CRYPT_MAX 160U

/*
 * Build a crypt string for `password` using `salt` (NXU_PW_SALT_LEN bytes) and
 * `iterations`, writing it NUL-terminated into out (capacity bytes). Returns 0
 * on success, -1 if it would not fit.
 */
int nxu_pwhash_make(const char *password, const uint8_t *salt, uint32_t iterations, char *out, uint32_t capacity);

/*
 * Check `password` against a stored crypt string. Returns 1 on a match, 0 on a
 * mismatch, -1 if the crypt string is malformed. Constant-time in the digest
 * comparison.
 */
int nxu_pwverify(const char *password, const char *crypt);

/*
 * Authenticate `name` with `password` against /etc/shadow. Returns 1 on
 * success, 0 on failure (wrong password, no entry, or unreadable file).
 */
int nxu_authenticate(const char *name, const char *password);

/*
 * Fill salt with NXU_PW_SALT_LEN bytes for a new password. A salt need only be
 * unique, not secret; this derives it from boot time, PID and timing jitter,
 * hashed. Returns 0.
 */
int nxu_pwsalt(uint8_t *salt);

/*
 * Set `name`'s crypt string in /etc/shadow, replacing the existing entry or
 * appending one. The caller must be able to write /etc/shadow (i.e. be root).
 * Returns 0 on success, -1 on failure.
 */
int nxu_shadow_set(const char *name, const char *crypt);

#endif
