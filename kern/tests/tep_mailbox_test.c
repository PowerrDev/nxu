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

#include <drivers/tep/tep_mailbox.h>
#include <kern/console/console.h>
#include <kern/machine/cpu.h>
#include <kern/machine/timer.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

#define TEP_TEST_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

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
	return tep_mailbox_test_expect(TEP_MB_CMD_GET_HEALTH, 0U, TEP_MB_OK, &response);
}

bool tep_mailbox_test_run(void)
{
	if (tep_mailbox_link_state() == TEP_LINK_ABSENT) {
		TEP_TEST_LOG("no mailbox UART: boot QEMU with a second -serial connected to tepOS\n");
		return false;
	}

	if (!tep_mailbox_test_protocol()) return false;
	kputln("tep_mailbox_test: protocol checks passed");

	if (!tep_mailbox_test_fail_closed()) return false;
	kputln("tep_mailbox_test: fail-closed check passed");

	if (!tep_mailbox_test_recovery()) return false;
	kputln("tep_mailbox_test: reconnect passed");
	return true;
}

#endif
