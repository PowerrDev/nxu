/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/tep_mailbox_test.c
 *
 * See kern/tests/tep_mailbox_test.h. Built only into the tep-mailbox test
 * kernel (NXU_TEP_MAILBOX_TEST): linking this unused code into the other
 * kernels made the `everything` row lose its UIService event loop, a
 * layout-sensitive failure not yet understood (see doc/drivers/tep-mailbox.md).
 */

#include <kern/tests/tep_mailbox_test.h>

#if defined(NXU_TEP_MAILBOX_TEST)

#include <drivers/tep/tep_crypto.h>
#include <drivers/tep/tep_mailbox.h>
#include <kern/console/console.h>
#include <kern/machine/cpu.h>
#include <kern/machine/timer.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEP_TEST_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

/* A key made before the harness restarts tepOS, checked again after it. */
static uint32_t g_key_handle;
static uint8_t g_key_public[TEP_PUBLIC_KEY_SIZE];

/* Runs on the boot thread before the periodic timer: wait by yielding to the monitor thread. */
static bool tep_mailbox_test_wait_for(tep_link_state_t wanted, bool equal, uint64_t seconds)
{
	uint64_t deadline = timer_get_microseconds() + seconds * 1000000ULL;

	while (timer_get_microseconds() < deadline) {
		if ((tep_mailbox_link_state() == wanted) == equal) return true;
		if (!sched_yield()) cpu_relax();
	}
	return false;
}

static void tep_mailbox_test_pause_ms(uint64_t milliseconds)
{
	uint64_t deadline = timer_get_microseconds() + milliseconds * 1000ULL;

	while (timer_get_microseconds() < deadline) {
		if (!sched_yield()) cpu_relax();
	}
}

static bool tep_mailbox_test_expect(uint16_t command, uint16_t payload_len, tep_mb_status_t expected, tep_mailbox_response_t *response)
{
	static const uint8_t junk[2] = { 1U, 2U };
	tep_request_result_t result = tep_mailbox_request(command, payload_len != 0U ? junk : 0, payload_len, response);

	if (result != TEP_REQ_OK) {
		TEP_TEST_LOG("command 0x%x: request %s\n", (unsigned)command, tep_request_result_name(result));
		return false;
	}
	if (response->status != (uint16_t)expected) {
		TEP_TEST_LOG("command 0x%x: status %u, expected %u\n", (unsigned)command, (unsigned)response->status, (unsigned)expected);
		return false;
	}
	return true;
}

static bool tep_mailbox_test_protocol(void)
{
	tep_mailbox_response_t response;
	tep_mb_health_t health = TEP_MB_HEALTH_STARTING;

	if (!tep_mailbox_test_wait_for(TEP_LINK_AVAILABLE, true, 60ULL)) {
		TEP_TEST_LOG("tepOS did not become available within 60 s\n");
		return false;
	}

	if (!tep_mailbox_test_expect(TEP_MB_CMD_HELLO, 0U, TEP_MB_OK, &response)) return false;
	if (response.payload_len != TEP_MB_HELLO_LEN || response.payload[0] != TEP_MB_VERSION) {
		TEP_TEST_LOG("HELLO response malformed\n");
		return false;
	}
	TEP_TEST_LOG("HELLO ok\n");

	/* tepOS may still be starting its services: give it a few health periods. */
	for (uint32_t attempt = 0U; attempt < 20U; attempt++) {
		if (!tep_mailbox_test_expect(TEP_MB_CMD_GET_HEALTH, 0U, TEP_MB_OK, &response)) return false;
		health = (tep_mb_health_t)response.payload[0];
		if (health == TEP_MB_HEALTH_OK) break;
		tep_mailbox_test_pause_ms(500ULL);
	}
	if (health != TEP_MB_HEALTH_OK) {
		TEP_TEST_LOG("tepOS health stayed %s\n", tep_mb_health_name((uint8_t)health));
		return false;
	}
	TEP_TEST_LOG("tepOS health ok with %u service(s)\n", (unsigned)response.payload[1]);

	if (!tep_mailbox_test_expect(0x7777U, 0U, TEP_MB_BAD_COMMAND, &response)) return false;
	TEP_TEST_LOG("unknown command refused\n");

	if (!tep_mailbox_test_expect(TEP_MB_CMD_GET_HEALTH, 2U, TEP_MB_BAD_LENGTH, &response)) return false;
	TEP_TEST_LOG("wrong payload length refused\n");
	return true;
}

/*
 * The crypto services through drivers/tep/tep_crypto.c. Signatures are
 * checked for size here; tepOS's own tests verify them with an independent
 * Ed25519 implementation.
 */
static bool tep_mailbox_test_crypto(void)
{
	static const uint8_t abc_digest[TEP_SHA256_SIZE] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
	};
	uint8_t digest[TEP_SHA256_SIZE], random_a[32], random_b[32], signature[TEP_SIGNATURE_SIZE];
	tep_result_t result;

	if ((result = tep_sha256("abc", 3U, digest)) != TEP_OK || memcmp(digest, abc_digest, sizeof(digest)) != 0) {
		TEP_TEST_LOG("SHA-256 of abc wrong (%s)\n", tep_result_name(result));
		return false;
	}
	TEP_TEST_LOG("SHA-256 matches the FIPS 180-4 vector\n");

	if (tep_random(random_a, sizeof(random_a)) != TEP_OK || tep_random(random_b, sizeof(random_b)) != TEP_OK ||
		memcmp(random_a, random_b, sizeof(random_a)) == 0) {
		TEP_TEST_LOG("random bytes missing or repeated\n");
		return false;
	}
	TEP_TEST_LOG("random bytes ok\n");

	if ((result = tep_key_generate(&g_key_handle)) != TEP_OK ||
		(result = tep_key_public(g_key_handle, g_key_public)) != TEP_OK ||
		(result = tep_key_sign(g_key_handle, "NXU", 3U, signature)) != TEP_OK) {
		TEP_TEST_LOG("key generate/public/sign failed (%s)\n", tep_result_name(result));
		return false;
	}
	TEP_TEST_LOG("key 0x%x generated, public key and signature returned\n", (unsigned)g_key_handle);

	if ((result = tep_key_public(g_key_handle ^ 0xffffffffU, g_key_public)) != TEP_ERR_NOT_FOUND) {
		TEP_TEST_LOG("unknown handle gave %s\n", tep_result_name(result));
		return false;
	}
	/* The call above must not have written: fetch the key again. */
	return tep_key_public(g_key_handle, g_key_public) == TEP_OK;
}

static bool tep_mailbox_test_fail_closed(void)
{
	tep_mailbox_response_t response;
	tep_mb_health_t health;

	TEP_TEST_LOG("waiting for tepOS to go away (the harness stops it)\n");
	if (!tep_mailbox_test_wait_for(TEP_LINK_AVAILABLE, false, 120ULL)) {
		TEP_TEST_LOG("tepOS never became unavailable\n");
		return false;
	}

	tep_request_result_t result = tep_mailbox_request(TEP_MB_CMD_GET_HEALTH, 0, 0U, &response);

	if (result != TEP_REQ_UNAVAILABLE) {
		TEP_TEST_LOG("request while unavailable returned %s\n", tep_request_result_name(result));
		return false;
	}
	uint8_t digest[TEP_SHA256_SIZE];
	tep_result_t crypto = tep_sha256("x", 1U, digest);
	if (crypto != TEP_ERR_UNAVAILABLE) {
		TEP_TEST_LOG("SHA-256 while unavailable returned %s\n", tep_result_name(crypto));
		return false;
	}
	if (tep_mailbox_health(&health)) {
		TEP_TEST_LOG("stale health reported while unavailable\n");
		return false;
	}
	return true;
}

static bool tep_mailbox_test_recovery(void)
{
	tep_mailbox_response_t response;

	TEP_TEST_LOG("waiting for tepOS to come back (the harness restarts it)\n");
	if (!tep_mailbox_test_wait_for(TEP_LINK_AVAILABLE, true, 120ULL)) {
		TEP_TEST_LOG("tepOS did not come back\n");
		return false;
	}
	if (!tep_mailbox_test_expect(TEP_MB_CMD_GET_HEALTH, 0U, TEP_MB_OK, &response)) return false;

	/* The harness restarts tepOS on the same (scratch) key store: the key must be there. */
	uint8_t public_key[TEP_PUBLIC_KEY_SIZE];
	tep_result_t result = tep_key_public(g_key_handle, public_key);
	if (result != TEP_OK || memcmp(public_key, g_key_public, sizeof(public_key)) != 0) {
		TEP_TEST_LOG("key 0x%x after tepOS restart: %s\n", (unsigned)g_key_handle, tep_result_name(result));
		return false;
	}
	TEP_TEST_LOG("key survived the tepOS restart\n");
	return tep_key_delete(g_key_handle) == TEP_OK;
}

bool tep_mailbox_test_run(void)
{
	if (tep_mailbox_link_state() == TEP_LINK_ABSENT) {
		TEP_TEST_LOG("no mailbox UART: boot QEMU with a second -serial connected to tepOS\n");
		return false;
	}

	if (!tep_mailbox_test_protocol()) return false;
	if (!tep_mailbox_test_crypto()) return false;
	kputln("tep_mailbox_test: protocol checks passed");

	if (!tep_mailbox_test_fail_closed()) return false;
	kputln("tep_mailbox_test: fail-closed check passed");

	if (!tep_mailbox_test_recovery()) return false;
	kputln("tep_mailbox_test: reconnect passed");
	return true;
}

#endif
