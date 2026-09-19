#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Throwaway cross-process IPC test, sender side. Paired with ipctest_a.c;
 * both are spawned directly by kern/tests/ipc_process_test.c after it has
 * confirmed bootd's bootstrap registry is up. Discovers ipctest_a's port by
 * name through the registry instead of relying on a hardcoded rendezvous
 * name.
 */

#define IPCTEST_LABEL "com.nxu.test.ipctest_a"
#define IPCTEST_BUFFER_SIZE 64U

static void
ipctest_b_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

static bool
ipctest_b_payload_matches(const char *buffer, int64_t length, const char *expected)
{
	uint64_t expected_length = nxu_strlen(expected);
	if ((uint64_t)length != expected_length) return false;

	for (uint64_t index = 0ULL; index < expected_length; index++) {
		if (buffer[index] != expected[index]) return false;
	}

	return true;
}

int
main(void)
{
	uint32_t target_name = 0U;
	if (!bootstrap_client_lookup(IPCTEST_LABEL, &target_name)) {
		ipctest_b_log("ipctest_b: bootstrap lookup failed\n");
		return 1;
	}

	ipctest_b_log("ipctest_b: found " IPCTEST_LABEL " via bootstrap registry\n");

	int64_t reply_name = nxu_ipc_port_allocate();
	if (reply_name <= 0) {
		ipctest_b_log("ipctest_b: reply port allocation failed\n");
		return 2;
	}

	ipctest_b_log("ipctest_b: sending \"ping\" with reply port transferred\n");

	const char ping[] = "ping";
	if (nxu_ipc_send(target_name, ping, sizeof(ping) - 1U, (uint32_t)reply_name) != 0) {
		ipctest_b_log("ipctest_b: send failed\n");
		return 3;
	}

	char buffer[IPCTEST_BUFFER_SIZE];
	int64_t received;

	for (;;) {
		received = nxu_ipc_receive((uint32_t)reply_name, buffer, sizeof(buffer), 0, 0);
		if (received >= 0) break;
		if (received != -NXU_SYS_E_AGAIN) return 4;
		(void)nxu_yield();
	}

	if (!ipctest_b_payload_matches(buffer, received, "pong")) {
		ipctest_b_log("ipctest_b: unexpected reply payload\n");
		return 5;
	}

	ipctest_b_log("ipctest_b: received \"pong\", exiting\n");
	return 0;
}
