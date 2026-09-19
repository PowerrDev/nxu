#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Throwaway cross-process IPC test, receiver side. Paired with ipctest_b.c;
 * both are spawned directly by kern/tests/ipc_process_test.c after it has
 * confirmed bootd's bootstrap registry is up. Registers its own port under
 * a well-known label instead of relying on a hardcoded rendezvous name.
 */

#define IPCTEST_LABEL "com.nxu.test.ipctest_a"
#define IPCTEST_BUFFER_SIZE 64U

static void
ipctest_a_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

static bool
ipctest_a_payload_matches(const char *buffer, int64_t length, const char *expected)
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
	int64_t service_name = nxu_ipc_port_allocate();
	if (service_name <= 0) {
		ipctest_a_log("ipctest_a: service port allocation failed\n");
		return 1;
	}

	if (!bootstrap_client_register(IPCTEST_LABEL, (uint32_t)service_name)) {
		ipctest_a_log("ipctest_a: bootstrap registration failed\n");
		return 2;
	}

	ipctest_a_log("ipctest_a: registered as " IPCTEST_LABEL ", waiting for a client\n");

	char buffer[IPCTEST_BUFFER_SIZE];
	uint32_t xfer_name = 0U;
	uint32_t xfer_type = 0U;
	int64_t received;

	for (;;) {
		received = nxu_ipc_receive((uint32_t)service_name, buffer, sizeof(buffer), &xfer_name, &xfer_type);
		if (received >= 0) break;
		if (received != -NXU_SYS_E_AGAIN) return 3;
		(void)nxu_yield();
	}

	if (xfer_type != NXU_IPC_XFER_PORT || xfer_name == 0U) {
		ipctest_a_log("ipctest_a: message carried no reply port\n");
		return 4;
	}

	if (!ipctest_a_payload_matches(buffer, received, "ping")) {
		ipctest_a_log("ipctest_a: unexpected payload\n");
		return 5;
	}

	ipctest_a_log("ipctest_a: received \"ping\" with reply port transferred, sending \"pong\"\n");

	const char reply[] = "pong";
	if (nxu_ipc_send(xfer_name, reply, sizeof(reply) - 1U, 0U) != 0) {
		ipctest_a_log("ipctest_a: reply send failed\n");
		return 6;
	}

	ipctest_a_log("ipctest_a: reply sent, exiting\n");
	return 0;
}
