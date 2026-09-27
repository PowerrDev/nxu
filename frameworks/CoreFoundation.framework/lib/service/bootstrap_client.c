/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/CoreFoundation.framework/lib/service/bootstrap_client.c
 *
 * Userspace client for bootd's bootstrap registry. See
 * frameworks/include/nxu/bootstrap_protocol.h for the wire format and
 * frameworks/CoreFoundation.framework/sbin/bootd/registry.c for the server.
 */

#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

#include <nxu/bootstrap_protocol.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * bootd's registry answers a lookup once, immediately, with whatever it
 * knows *right now* (see bootd_registry_handle_lookup) -- there is no
 * "queue this and answer later" logic there. A target that has not
 * registered yet by the moment its lookup is drained gets an honest,
 * immediate NOT_FOUND, not a stall. Absorbing another process's startup
 * latency is therefore this client's job: resend a fresh lookup after a
 * NOT_FOUND, not just keep waiting on the reply to the first one.
 *
 * Deadlines here are wall-clock (nxu_uptime_us), not iteration counts: how
 * long a fixed number of nxu_yield() spins takes in real time varies wildly
 * with host load and whether QEMU is running with hardware acceleration --
 * bootd's own job spawning is synchronous and can block on slow disk I/O
 * for several real seconds per job (see job.c's image byte-compare), during
 * which nothing else runs at all. An iteration budget tuned for one machine
 * can be exhausted in a fraction of a second on a slower one.
 */
#define BOOTSTRAP_CLIENT_LOOKUP_DEADLINE_US 120000000ULL
#define BOOTSTRAP_CLIENT_LOOKUP_REPLY_DEADLINE_US 3000000ULL
#define BOOTSTRAP_CLIENT_REGISTER_DEADLINE_US 30000000ULL
#define BOOTSTRAP_CLIENT_RETRY_US 10000ULL

/* Wall-clock-agnostic fallback for the rare case nxu_uptime_us() itself is
 * unavailable (returns < 0) -- bound by iterations instead of giving up
 * immediately or looping forever. */
#define BOOTSTRAP_CLIENT_FALLBACK_ATTEMPTS 2000000U

static bool
bootstrap_client_deadline_passed(int64_t start_us, uint64_t deadline_us, uint32_t fallback_attempt, uint32_t fallback_limit)
{
	if (start_us < 0) return fallback_attempt >= fallback_limit;

	int64_t now_us = nxu_uptime_us();
	if (now_us < 0) return fallback_attempt >= fallback_limit;

	return (uint64_t)(now_us - start_us) >= deadline_us;
}

static bool
bootstrap_client_fill_label(char destination[NXU_BOOTSTRAP_LABEL_MAX], const char *label)
{
	uint64_t length = nxu_strlen(label);
	if (length >= NXU_BOOTSTRAP_LABEL_MAX) return false;

	for (uint64_t index = 0ULL; index < length; index++) destination[index] = label[index];
	for (uint64_t index = length; index < NXU_BOOTSTRAP_LABEL_MAX; index++) destination[index] = '\0';
	return true;
}

bool
bootstrap_client_register(const char *label, uint32_t service_port_name)
{
	if (label == 0 || service_port_name == 0U) return false;

	nxu_bootstrap_request_t request;
	request.opcode = (uint32_t)NXU_BOOTSTRAP_OP_REGISTER;
	if (!bootstrap_client_fill_label(request.label, label)) return false;

	/*
	 * nxu_ipc_bootstrap_port()/nxu_ipc_send() have no reason to fail once
	 * this process is actually running, but this is a one-shot call with
	 * no process-level retry behind it (bootd only relaunches KeepAlive
	 * jobs, and this may not be one) -- retry for a bounded real-time
	 * window rather than letting a single transient failure be fatal.
	 */
	int64_t start_us = nxu_uptime_us();
	for (uint32_t fallback_attempt = 0U; ; fallback_attempt++) {
		int64_t bootstrap_name = nxu_ipc_bootstrap_port();
		if (bootstrap_name > 0 && nxu_ipc_send((uint32_t)bootstrap_name, &request, sizeof(request), service_port_name) == 0) {
			return true;
		}
		if (bootstrap_client_deadline_passed(start_us, BOOTSTRAP_CLIENT_REGISTER_DEADLINE_US, fallback_attempt, BOOTSTRAP_CLIENT_FALLBACK_ATTEMPTS)) break;
		(void)nxu_yield();
	}

	return false;
}

bool
bootstrap_client_lookup(const char *label, uint32_t *out_port_name)
{
	if (out_port_name != 0) *out_port_name = 0U;
	if (label == 0 || out_port_name == 0) return false;

	int64_t bootstrap_name = nxu_ipc_bootstrap_port();
	if (bootstrap_name <= 0) return false;

	int64_t reply_name = nxu_ipc_port_allocate();
	if (reply_name <= 0) return false;

	nxu_bootstrap_request_t request;
	request.opcode = (uint32_t)NXU_BOOTSTRAP_OP_LOOKUP;
	if (!bootstrap_client_fill_label(request.label, label)) {
		(void)nxu_ipc_port_deallocate((uint32_t)reply_name);
		return false;
	}

	bool found = false;
	uint32_t xfer_name = 0U;
	int64_t start_us = nxu_uptime_us();

	for (uint32_t fallback_attempt = 0U; !bootstrap_client_deadline_passed(start_us, BOOTSTRAP_CLIENT_LOOKUP_DEADLINE_US, fallback_attempt, BOOTSTRAP_CLIENT_FALLBACK_ATTEMPTS); fallback_attempt++) {
		if (nxu_ipc_send((uint32_t)bootstrap_name, &request, sizeof(request), (uint32_t)reply_name) != 0) break;

		nxu_bootstrap_reply_t reply;
		uint32_t xfer_type = 0U;
		int64_t received = -1;
		int64_t attempt_start_us = nxu_uptime_us();

		for (uint32_t reply_fallback = 0U; ; reply_fallback++) {
			received = nxu_ipc_receive((uint32_t)reply_name, &reply, sizeof(reply), &xfer_name, &xfer_type);
			if (received >= 0) break;
			if (received != -NXU_SYS_E_AGAIN) {
				(void)nxu_ipc_port_deallocate((uint32_t)reply_name);
				return false;
			}
			if (bootstrap_client_deadline_passed(attempt_start_us, BOOTSTRAP_CLIENT_LOOKUP_REPLY_DEADLINE_US, reply_fallback, BOOTSTRAP_CLIENT_FALLBACK_ATTEMPTS)) break;
			(void)nxu_yield();
		}

		if (
			received >= 0 && (uint64_t)received == sizeof(reply) &&
			reply.status == (int32_t)NXU_BOOTSTRAP_STATUS_OK &&
			xfer_type == NXU_IPC_XFER_PORT && xfer_name != 0U
		) {
			found = true;
			break;
		}

		/*
		 * Not registered yet, or this attempt's reply never showed up
		 * within BOOTSTRAP_CLIENT_LOOKUP_REPLY_DEADLINE_US -- either way,
		 * send a fresh lookup rather than giving up: bootd answers with
		 * whatever it knows at the moment it drains the message, so a
		 * stale NOT_FOUND only means the target hadn't registered *yet*,
		 * not that it never will.
		 */
		xfer_name = 0U;
		/* Give the target a moment to register before asking again (bootd answers every 10 ms or so). */
		(void)nxu_sleep_us(BOOTSTRAP_CLIENT_RETRY_US);
	}

	(void)nxu_ipc_port_deallocate((uint32_t)reply_name);

	if (!found) return false;

	*out_port_name = xfer_name;
	return true;
}
