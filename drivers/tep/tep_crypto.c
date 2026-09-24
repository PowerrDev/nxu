/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_crypto.c
 *
 * See drivers/tep/tep_crypto.h.
 */

#include <drivers/tep/tep_crypto.h>

#include <drivers/tep/tep_mailbox.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8U);
	p[2] = (uint8_t)(value >> 16U);
	p[3] = (uint8_t)(value >> 24U);
}

static tep_result_t tep_from_status(uint16_t status)
{
	switch (status) {
	case TEP_MB_OK: return TEP_OK;
	case TEP_MB_NOT_FOUND: return TEP_ERR_NOT_FOUND;
	case TEP_MB_FULL: return TEP_ERR_FULL;
	case TEP_MB_UNAVAILABLE: return TEP_ERR_UNAVAILABLE;
	case TEP_MB_BAD_LENGTH: return TEP_ERR_INVALID;
	default: return TEP_ERR_PROTOCOL;
	}
}

/*
 * tep_crypto_call:
 *
 * One crypto command. Refused without crypto support in tepOS; succeeds only
 * with status OK and exactly want_len bytes of payload.
 */
static tep_result_t tep_crypto_call(uint16_t command, const void *payload, uint16_t payload_len, tep_mailbox_response_t *response, uint16_t want_len)
{
	if ((tep_mailbox_features() & TEP_MB_FEATURE_CRYPTO) == 0U) return TEP_ERR_UNAVAILABLE;

	tep_request_result_t result = tep_mailbox_request(command, payload, payload_len, response);

	if (result == TEP_REQ_UNAVAILABLE || result == TEP_REQ_TIMEOUT) return TEP_ERR_UNAVAILABLE;
	if (result != TEP_REQ_OK) return TEP_ERR_PROTOCOL;
	if (response->status != TEP_MB_OK) return tep_from_status(response->status);
	if (response->payload_len != want_len) return TEP_ERR_PROTOCOL;
	return TEP_OK;
}

tep_result_t tep_sha256(const void *data, uint32_t size, uint8_t digest[TEP_SHA256_SIZE])
{
	tep_mailbox_response_t response;

	if (data == 0 || digest == 0 || size == 0U || size > TEP_MB_SHA256_MAX) return TEP_ERR_INVALID;

	tep_result_t result = tep_crypto_call(TEP_MB_CMD_SHA256, data, (uint16_t)size, &response, TEP_SHA256_SIZE);

	if (result == TEP_OK) memcpy(digest, response.payload, TEP_SHA256_SIZE);
	return result;
}

tep_result_t tep_random(void *out, uint32_t size)
{
	tep_mailbox_response_t response;
	uint8_t request[2];

	if (out == 0 || size == 0U || size > TEP_MB_RANDOM_MAX) return TEP_ERR_INVALID;

	request[0] = (uint8_t)size;
	request[1] = (uint8_t)(size >> 8U);

	tep_result_t result = tep_crypto_call(TEP_MB_CMD_RANDOM, request, sizeof(request), &response, (uint16_t)size);

	if (result == TEP_OK) memcpy(out, response.payload, size);
	return result;
}

tep_result_t tep_key_generate(uint32_t *handle)
{
	tep_mailbox_response_t response;
	uint8_t algorithm = TEP_MB_ALG_ED25519;

	if (handle == 0) return TEP_ERR_INVALID;

	tep_result_t result = tep_crypto_call(TEP_MB_CMD_KEY_GENERATE, &algorithm, 1U, &response, 4U);

	if (result == TEP_OK) {
		*handle = (uint32_t)response.payload[0] | (uint32_t)response.payload[1] << 8U |
			(uint32_t)response.payload[2] << 16U | (uint32_t)response.payload[3] << 24U;
	}
	return result;
}

tep_result_t tep_key_public(uint32_t handle, uint8_t public_key[TEP_PUBLIC_KEY_SIZE])
{
	tep_mailbox_response_t response;
	uint8_t request[4];

	if (public_key == 0) return TEP_ERR_INVALID;
	put32(request, handle);

	tep_result_t result = tep_crypto_call(TEP_MB_CMD_KEY_PUBLIC, request, sizeof(request), &response, TEP_PUBLIC_KEY_SIZE);

	if (result == TEP_OK) memcpy(public_key, response.payload, TEP_PUBLIC_KEY_SIZE);
	return result;
}

tep_result_t tep_key_sign(uint32_t handle, const void *message, uint32_t size, uint8_t signature[TEP_SIGNATURE_SIZE])
{
	tep_mailbox_response_t response;
	uint8_t request[4U + TEP_MB_SIGN_MAX];

	if (message == 0 || signature == 0 || size == 0U || size > TEP_MB_SIGN_MAX) return TEP_ERR_INVALID;
	put32(request, handle);
	memcpy(request + 4U, message, size);

	tep_result_t result = tep_crypto_call(TEP_MB_CMD_KEY_SIGN, request, (uint16_t)(4U + size), &response, TEP_SIGNATURE_SIZE);

	if (result == TEP_OK) memcpy(signature, response.payload, TEP_SIGNATURE_SIZE);
	return result;
}

tep_result_t tep_key_delete(uint32_t handle)
{
	tep_mailbox_response_t response;
	uint8_t request[4];

	put32(request, handle);
	return tep_crypto_call(TEP_MB_CMD_KEY_DELETE, request, sizeof(request), &response, 0U);
}

const char *tep_result_name(tep_result_t result)
{
	switch (result) {
	case TEP_OK: return "ok";
	case TEP_ERR_UNAVAILABLE: return "tepOS unavailable";
	case TEP_ERR_INVALID: return "invalid arguments";
	case TEP_ERR_NOT_FOUND: return "no such key";
	case TEP_ERR_FULL: return "key store full";
	case TEP_ERR_PROTOCOL: return "protocol error";
	}
	return "unknown";
}
