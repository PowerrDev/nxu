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
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8U);
	p[2] = (uint8_t)(value >> 16U);
	p[3] = (uint8_t)(value >> 24U);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8U | (uint32_t)p[2] << 16U | (uint32_t)p[3] << 24U;
}

/* Clears a buffer that held a passcode; volatile so the stores are not dropped as dead. */
static void tep_wipe(void *buffer, size_t size)
{
	volatile uint8_t *bytes = buffer;

	for (size_t index = 0U; index < size; index++) bytes[index] = 0U;
}

static tep_result_t tep_from_status(uint16_t status)
{
	switch (status) {
	case TEP_MB_OK: return TEP_OK;
	case TEP_MB_NOT_FOUND: return TEP_ERR_NOT_FOUND;
	case TEP_MB_FULL: return TEP_ERR_FULL;
	case TEP_MB_UNAVAILABLE: return TEP_ERR_UNAVAILABLE;
	case TEP_MB_BAD_LENGTH: return TEP_ERR_INVALID;
	case TEP_MB_DENIED: return TEP_ERR_DENIED;
	case TEP_MB_RETRY_LATER: return TEP_ERR_RETRY_LATER;
	case TEP_MB_LOCKED: return TEP_ERR_LOCKED;
	case TEP_MB_ROLLBACK: return TEP_ERR_ROLLBACK;
	default: return TEP_ERR_PROTOCOL;
	}
}

/*
 * tep_service_call:
 *
 * One command of a feature. Refused when tepOS does not offer the feature;
 * succeeds only with status OK and exactly want_len bytes of payload. Other
 * statuses leave their payload in *response for the caller to check.
 */
static tep_result_t tep_service_call(uint16_t feature, uint16_t command, const void *payload, uint16_t payload_len, tep_mailbox_response_t *response, uint16_t want_len)
{
	if ((tep_mailbox_features() & feature) == 0U) return TEP_ERR_UNAVAILABLE;

	tep_request_result_t result = tep_mailbox_request(command, payload, payload_len, response);

	if (result == TEP_REQ_UNAVAILABLE || result == TEP_REQ_TIMEOUT) return TEP_ERR_UNAVAILABLE;
	if (result != TEP_REQ_OK) return TEP_ERR_PROTOCOL;
	if (response->status != TEP_MB_OK) return tep_from_status(response->status);
	if (response->payload_len != want_len) return TEP_ERR_PROTOCOL;
	return TEP_OK;
}

static tep_result_t tep_crypto_call(uint16_t command, const void *payload, uint16_t payload_len, tep_mailbox_response_t *response, uint16_t want_len)
{
	return tep_service_call(TEP_MB_FEATURE_CRYPTO, command, payload, payload_len, response, want_len);
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

	if (result == TEP_OK) *handle = get32(response.payload);
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

static bool tep_passcode_size_ok(uint32_t size)
{
	return size >= TEP_MB_PASSCODE_MIN && size <= TEP_MB_PASSCODE_MAX;
}

/*
 * The end of AUTH_SET and AUTH_VERIFY: RETRY_LATER carries a u32, DENIED at
 * most the u8 failure count (unused here: tep_auth_status has it), LOCKED and
 * NOT_FOUND nothing.
 */
static tep_result_t tep_auth_finish(tep_result_t result, const tep_mailbox_response_t *response, uint32_t *wait_seconds)
{
	if (result == TEP_ERR_RETRY_LATER) {
		if (response->payload_len != 4U) return TEP_ERR_PROTOCOL;
		if (wait_seconds != 0) *wait_seconds = get32(response->payload);
	} else if (result == TEP_ERR_DENIED && response->payload_len > 1U) {
		return TEP_ERR_PROTOCOL;
	} else if ((result == TEP_ERR_LOCKED || result == TEP_ERR_NOT_FOUND) && response->payload_len != 0U) {
		return TEP_ERR_PROTOCOL;
	}
	return result;
}

tep_result_t tep_auth_set(const void *old_passcode, uint32_t old_size, const void *passcode, uint32_t size, uint32_t *wait_seconds)
{
	tep_mailbox_response_t response;
	uint8_t request[1U + 2U * TEP_MB_PASSCODE_MAX];

	if (wait_seconds != 0) *wait_seconds = 0U;
	if (passcode == 0 || !tep_passcode_size_ok(size)) return TEP_ERR_INVALID;
	if (old_size != 0U && (old_passcode == 0 || !tep_passcode_size_ok(old_size))) return TEP_ERR_INVALID;

	request[0] = (uint8_t)old_size;
	if (old_size != 0U) memcpy(request + 1U, old_passcode, old_size);
	memcpy(request + 1U + old_size, passcode, size);

	tep_result_t result = tep_service_call(TEP_MB_FEATURE_AUTH, TEP_MB_CMD_AUTH_SET, request, (uint16_t)(1U + old_size + size), &response, 0U);

	tep_wipe(request, sizeof(request));
	return tep_auth_finish(result, &response, wait_seconds);
}

tep_result_t tep_auth_verify(const void *passcode, uint32_t size, uint32_t *wait_seconds)
{
	tep_mailbox_response_t response;
	uint8_t request[TEP_MB_PASSCODE_MAX];

	if (wait_seconds != 0) *wait_seconds = 0U;
	if (passcode == 0 || !tep_passcode_size_ok(size)) return TEP_ERR_INVALID;

	memcpy(request, passcode, size);

	tep_result_t result = tep_service_call(TEP_MB_FEATURE_AUTH, TEP_MB_CMD_AUTH_VERIFY, request, (uint16_t)size, &response, 0U);

	tep_wipe(request, sizeof(request));
	return tep_auth_finish(result, &response, wait_seconds);
}

tep_result_t tep_auth_status(tep_auth_status_t *status)
{
	tep_mailbox_response_t response;

	if (status == 0) return TEP_ERR_INVALID;

	tep_result_t result = tep_service_call(TEP_MB_FEATURE_AUTH, TEP_MB_CMD_AUTH_STATUS, 0, 0U, &response, TEP_MB_AUTH_STATUS_LEN);

	if (result != TEP_OK) return result;
	if (response.payload[0] > 1U || response.payload[2] > 1U) return TEP_ERR_PROTOCOL;

	status->passcode_set = response.payload[0] != 0U;
	status->failures = response.payload[1];
	status->locked = response.payload[2] != 0U;
	status->wait_seconds = get32(&response.payload[4]);
	return TEP_OK;
}

tep_result_t tep_boot_verify(const uint8_t manifest[TEP_BOOT_MANIFEST_SIZE], const uint8_t digest[TEP_SHA256_SIZE], uint32_t *detail)
{
	tep_mailbox_response_t response;
	uint8_t request[TEP_MB_BOOT_VERIFY_LEN];
	uint32_t value;

	if (detail != 0) *detail = 0U;
	if (manifest == 0 || digest == 0) return TEP_ERR_INVALID;

	memcpy(request, manifest, TEP_BOOT_MANIFEST_SIZE);
	memcpy(request + TEP_BOOT_MANIFEST_SIZE, digest, TEP_SHA256_SIZE);

	tep_result_t result = tep_service_call(TEP_MB_FEATURE_BOOT, TEP_MB_CMD_BOOT_VERIFY, request, sizeof(request), &response, 4U);

	if (result == TEP_OK || result == TEP_ERR_ROLLBACK) {
		if (response.payload_len != 4U) return TEP_ERR_PROTOCOL;
		value = get32(response.payload);
	} else if (result == TEP_ERR_DENIED) {
		if (response.payload_len != 1U) return TEP_ERR_PROTOCOL;
		value = response.payload[0];
	} else {
		return result;
	}

	if (detail != 0) *detail = value;
	return result;
}

const char *tep_boot_reason_name(uint32_t reason)
{
	switch (reason) {
	case TEP_BOOT_BAD_FORMAT: return "not a manifest";
	case TEP_BOOT_BAD_SIGNATURE: return "bad signature";
	case TEP_BOOT_WRONG_IMAGE: return "signed for another image";
	}
	return "unknown reason";
}

const char *tep_result_name(tep_result_t result)
{
	switch (result) {
	case TEP_OK: return "ok";
	case TEP_ERR_UNAVAILABLE: return "tepOS unavailable";
	case TEP_ERR_INVALID: return "invalid arguments";
	case TEP_ERR_NOT_FOUND: return "not found";
	case TEP_ERR_FULL: return "key store full";
	case TEP_ERR_PROTOCOL: return "protocol error";
	case TEP_ERR_DENIED: return "denied";
	case TEP_ERR_RETRY_LATER: return "retry later";
	case TEP_ERR_LOCKED: return "locked";
	case TEP_ERR_ROLLBACK: return "rollback";
	}
	return "unknown";
}
