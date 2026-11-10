/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * Password authentication: HMAC-SHA256, PBKDF2 over it, and the crypt-string
 * format stored in /etc/shadow. See nxu/auth.h.
 */
#include <nxu/auth.h>

#ifndef NXU_AUTH_HOST
#include <nxu/syscall.h>
#endif

#include <sha256.h>

#include <stdint.h>

static uint32_t
auth_strlen(const char *s)
{
	uint32_t n = 0U;
	while (s[n] != '\0') n++;
	return n;
}

/* HMAC-SHA256(key, message) -> out[32]. */
static void
hmac_sha256(const uint8_t *key, uint32_t key_len, const uint8_t *message, uint32_t message_len, uint8_t out[32])
{
	uint8_t padded[SHA256_BLOCK_SIZE];
	uint8_t inner_pad[SHA256_BLOCK_SIZE];
	uint8_t outer_pad[SHA256_BLOCK_SIZE];

	if (key_len > SHA256_BLOCK_SIZE) {
		sha256(key, key_len, padded);
		for (uint32_t i = SHA256_DIGEST_SIZE; i < SHA256_BLOCK_SIZE; i++) padded[i] = 0U;
	} else {
		for (uint32_t i = 0U; i < key_len; i++) padded[i] = key[i];
		for (uint32_t i = key_len; i < SHA256_BLOCK_SIZE; i++) padded[i] = 0U;
	}

	for (uint32_t i = 0U; i < SHA256_BLOCK_SIZE; i++) {
		inner_pad[i] = padded[i] ^ 0x36U;
		outer_pad[i] = padded[i] ^ 0x5CU;
	}

	uint8_t inner[SHA256_DIGEST_SIZE];
	sha256_context_t ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, inner_pad, SHA256_BLOCK_SIZE);
	sha256_update(&ctx, message, message_len);
	sha256_final(&ctx, inner);

	sha256_init(&ctx);
	sha256_update(&ctx, outer_pad, SHA256_BLOCK_SIZE);
	sha256_update(&ctx, inner, SHA256_DIGEST_SIZE);
	sha256_final(&ctx, out);
}

/*
 * PBKDF2-HMAC-SHA256 with a 32-byte derived key (one block, since the output
 * length equals the HMAC length).
 */
static void
pbkdf2_sha256(const char *password, const uint8_t *salt, uint32_t salt_len, uint32_t iterations, uint8_t out[32])
{
	uint32_t password_len = auth_strlen(password);

	/* First iteration runs over salt || INT32_BE(1). */
	uint8_t seed[NXU_PW_SALT_LEN + 4U];
	for (uint32_t i = 0U; i < salt_len; i++) seed[i] = salt[i];
	seed[salt_len + 0U] = 0U;
	seed[salt_len + 1U] = 0U;
	seed[salt_len + 2U] = 0U;
	seed[salt_len + 3U] = 1U;

	uint8_t u[32];
	uint8_t t[32];
	hmac_sha256((const uint8_t *)password, password_len, seed, salt_len + 4U, u);
	for (uint32_t i = 0U; i < 32U; i++) t[i] = u[i];

	for (uint32_t iter = 1U; iter < iterations; iter++) {
		hmac_sha256((const uint8_t *)password, password_len, u, 32U, u);
		for (uint32_t i = 0U; i < 32U; i++) t[i] ^= u[i];
	}

	for (uint32_t i = 0U; i < 32U; i++) out[i] = t[i];
}

static char
hex_digit(uint8_t nibble)
{
	return (char)(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U)));
}

static int
hex_value(char c, uint8_t *out)
{
	if (c >= '0' && c <= '9') {
		*out = (uint8_t)(c - '0');
		return 0;
	}
	if (c >= 'a' && c <= 'f') {
		*out = (uint8_t)(c - 'a' + 10);
		return 0;
	}
	if (c >= 'A' && c <= 'F') {
		*out = (uint8_t)(c - 'A' + 10);
		return 0;
	}
	return -1;
}

static void
to_hex(const uint8_t *bytes, uint32_t length, char *out)
{
	for (uint32_t i = 0U; i < length; i++) {
		out[i * 2U] = hex_digit((uint8_t)(bytes[i] >> 4U));
		out[i * 2U + 1U] = hex_digit((uint8_t)(bytes[i] & 0x0FU));
	}
	out[length * 2U] = '\0';
}

/* Parse exactly `length` bytes of hex from text; returns 0 on success. */
static int
from_hex(const char *text, uint32_t length, uint8_t *out)
{
	for (uint32_t i = 0U; i < length; i++) {
		uint8_t hi, lo;
		if (hex_value(text[i * 2U], &hi) != 0 || hex_value(text[i * 2U + 1U], &lo) != 0) return -1;
		out[i] = (uint8_t)((hi << 4U) | lo);
	}
	return 0;
}

static uint32_t
parse_u32(const char *text, const char **end)
{
	uint32_t value = 0U;
	while (*text >= '0' && *text <= '9') {
		value = value * 10U + (uint32_t)(*text - '0');
		text++;
	}
	*end = text;
	return value;
}

static const char CRYPT_PREFIX[] = "pbkdf2_sha256$";

int
nxu_pwhash_make(const char *password, const uint8_t *salt, uint32_t iterations, char *out, uint32_t capacity)
{
	uint8_t hash[NXU_PW_HASH_LEN];
	pbkdf2_sha256(password, salt, NXU_PW_SALT_LEN, iterations, hash);

	char salt_hex[NXU_PW_SALT_LEN * 2U + 1U];
	char hash_hex[NXU_PW_HASH_LEN * 2U + 1U];
	to_hex(salt, NXU_PW_SALT_LEN, salt_hex);
	to_hex(hash, NXU_PW_HASH_LEN, hash_hex);

	/* Compose: pbkdf2_sha256$<iters>$<salt-hex>$<hash-hex> */
	uint32_t pos = 0U;
	const char *prefix = CRYPT_PREFIX;
	while (*prefix != '\0') {
		if (pos + 1U >= capacity) return -1;
		out[pos++] = *prefix++;
	}

	/* iterations as decimal */
	char digits[11];
	int di = 10;
	digits[di] = '\0';
	uint32_t value = iterations;
	if (value == 0U) digits[--di] = '0';
	while (value != 0U) {
		digits[--di] = (char)('0' + (value % 10U));
		value /= 10U;
	}
	for (const char *d = &digits[di]; *d != '\0'; d++) {
		if (pos + 1U >= capacity) return -1;
		out[pos++] = *d;
	}

	if (pos + 1U >= capacity) return -1;
	out[pos++] = '$';
	for (const char *s = salt_hex; *s != '\0'; s++) {
		if (pos + 1U >= capacity) return -1;
		out[pos++] = *s;
	}
	if (pos + 1U >= capacity) return -1;
	out[pos++] = '$';
	for (const char *h = hash_hex; *h != '\0'; h++) {
		if (pos + 1U >= capacity) return -1;
		out[pos++] = *h;
	}
	out[pos] = '\0';
	return 0;
}

int
nxu_pwverify(const char *password, const char *crypt)
{
	/* Match the algorithm prefix. */
	const char *p = crypt;
	for (const char *q = CRYPT_PREFIX; *q != '\0'; q++, p++) {
		if (*p != *q) return -1;
	}

	const char *end;
	uint32_t iterations = parse_u32(p, &end);
	if (end == p || *end != '$' || iterations == 0U) return -1;
	p = end + 1;

	uint8_t salt[NXU_PW_SALT_LEN];
	if (from_hex(p, NXU_PW_SALT_LEN, salt) != 0) return -1;
	p += NXU_PW_SALT_LEN * 2U;
	if (*p != '$') return -1;
	p++;

	uint8_t stored[NXU_PW_HASH_LEN];
	if (from_hex(p, NXU_PW_HASH_LEN, stored) != 0) return -1;

	uint8_t computed[NXU_PW_HASH_LEN];
	pbkdf2_sha256(password, salt, NXU_PW_SALT_LEN, iterations, computed);

	uint8_t diff = 0U;
	for (uint32_t i = 0U; i < NXU_PW_HASH_LEN; i++) diff |= (uint8_t)(computed[i] ^ stored[i]);
	return diff == 0U ? 1 : 0;
}

#ifndef NXU_AUTH_HOST
int
nxu_authenticate(const char *name, const char *password)
{
	if (name == 0 || password == 0) return 0;

	int64_t fd = nxu_open(NXU_SHADOW_PATH, NXU_O_READ);
	if (fd < 0) return 0;

	char buffer[2048];
	uint64_t total = 0ULL;
	while (total + 1ULL < sizeof(buffer)) {
		int64_t count = nxu_read((uint64_t)fd, buffer + total, sizeof(buffer) - 1ULL - total);
		if (count <= 0) break;
		total += (uint64_t)count;
	}
	(void)nxu_close((uint64_t)fd);
	buffer[total] = '\0';

	uint32_t name_len = auth_strlen(name);

	char *line = buffer;
	while (*line != '\0') {
		/* A record is name:crypt:... up to the newline. */
		char *colon = line;
		while (*colon != '\0' && *colon != ':' && *colon != '\n') colon++;

		int name_matches = (*colon == ':') && ((uint32_t)(colon - line) == name_len);
		if (name_matches) {
			for (uint32_t i = 0U; i < name_len; i++) {
				if (line[i] != name[i]) {
					name_matches = 0;
					break;
				}
			}
		}

		char *crypt_begin = (*colon == ':') ? colon + 1 : colon;
		char *crypt_end = crypt_begin;
		while (*crypt_end != '\0' && *crypt_end != ':' && *crypt_end != '\n') crypt_end++;

		char *next = crypt_end;
		while (*next != '\0' && *next != '\n') next++;
		if (*next == '\n') next++;

		if (name_matches) {
			char crypt[NXU_CRYPT_MAX];
			uint32_t length = (uint32_t)(crypt_end - crypt_begin);
			if (length >= sizeof(crypt)) return 0;
			for (uint32_t i = 0U; i < length; i++) crypt[i] = crypt_begin[i];
			crypt[length] = '\0';
			return nxu_pwverify(password, crypt) == 1 ? 1 : 0;
		}

		line = next;
	}
	return 0;
}

int
nxu_pwsalt(uint8_t *salt)
{
	if (salt == 0) return -1;

	uint8_t material[72];
	uint32_t n = 0U;

	uint64_t values[9];
	values[0] = (uint64_t)nxu_getpid();
	values[1] = (uint64_t)nxu_uptime_us();
	for (uint32_t i = 2U; i < 9U; i++) {
		(void)nxu_sleep_us(1ULL);
		values[i] = (uint64_t)nxu_uptime_us();
	}
	for (uint32_t i = 0U; i < 9U; i++) {
		for (uint32_t b = 0U; b < 8U; b++) material[n++] = (uint8_t)(values[i] >> (b * 8U));
	}

	uint8_t digest[SHA256_DIGEST_SIZE];
	sha256(material, n, digest);
	for (uint32_t i = 0U; i < NXU_PW_SALT_LEN; i++) salt[i] = digest[i];
	return 0;
}

int
nxu_shadow_set(const char *name, const char *crypt)
{
	if (name == 0 || crypt == 0) return -1;

	int64_t fd = nxu_open(NXU_SHADOW_PATH, NXU_O_READ);
	char input[2048];
	uint64_t total = 0ULL;
	if (fd >= 0) {
		while (total + 1ULL < sizeof(input)) {
			int64_t count = nxu_read((uint64_t)fd, input + total, sizeof(input) - 1ULL - total);
			if (count <= 0) break;
			total += (uint64_t)count;
		}
		(void)nxu_close((uint64_t)fd);
	}
	input[total] = '\0';

	uint32_t name_len = auth_strlen(name);

	char output[2560];
	uint32_t out = 0U;
	int replaced = 0;

	/* Copy each line, substituting the target account's record. */
	const char *line = input;
	while (*line != '\0') {
		const char *end = line;
		while (*end != '\0' && *end != '\n') end++;

		const char *colon = line;
		while (colon < end && *colon != ':') colon++;
		int is_target = (uint32_t)(colon - line) == name_len;
		for (uint32_t i = 0U; is_target && i < name_len; i++)
			if (line[i] != name[i]) is_target = 0;

		if (is_target) {
			for (uint32_t i = 0U; name[i] != '\0' && out + 1U < sizeof(output); i++) output[out++] = name[i];
			if (out + 1U < sizeof(output)) output[out++] = ':';
			for (uint32_t i = 0U; crypt[i] != '\0' && out + 1U < sizeof(output); i++) output[out++] = crypt[i];
			replaced = 1;
		} else {
			for (const char *p = line; p < end && out + 1U < sizeof(output); p++) output[out++] = *p;
		}
		if (out + 1U < sizeof(output)) output[out++] = '\n';

		line = (*end == '\n') ? end + 1 : end;
	}

	if (!replaced) {
		for (uint32_t i = 0U; name[i] != '\0' && out + 1U < sizeof(output); i++) output[out++] = name[i];
		if (out + 1U < sizeof(output)) output[out++] = ':';
		for (uint32_t i = 0U; crypt[i] != '\0' && out + 1U < sizeof(output); i++) output[out++] = crypt[i];
		if (out + 1U < sizeof(output)) output[out++] = '\n';
	}

	int64_t wfd = nxu_open(NXU_SHADOW_PATH, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (wfd < 0) return -1;
	int ok = nxu_write((uint64_t)wfd, output, out) == (int64_t)out;
	(void)nxu_close((uint64_t)wfd);
	return ok ? 0 : -1;
}
#endif
