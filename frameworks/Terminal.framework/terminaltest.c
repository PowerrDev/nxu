/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include "netutil.h"

#include <nxu/string.h>
#include <nxu/syscall.h>

#define TERMINAL_TEST_BYTES 12288U

static int
terminaltest_child(void)
{
	char block[256];
	memset(block, 'x', sizeof(block));

	for (uint32_t written = 0; written < TERMINAL_TEST_BYTES; written += sizeof(block)) {
		if (nxu_write(1, block, sizeof(block)) != sizeof(block)) {
			return 1;
		}
	}

	/* Closing one duplicated descriptor must leave the other alive. */
	if (nxu_close(1) != 0 || nxu_write(2, "stderr\n", 7) != 7) {
		return 2;
	}

	return 7;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc == 2 && nxu_streq(argv[1], "--child")) {
		return terminaltest_child();
	}

	char name[64] = "com.nxu.terminaltest.";
	uint32_t offset = nxu_strlen(name);
	uint32_t pid = nxu_getpid();

	for (int32_t shift = 28; shift >= 0; shift -= 4) {
		name[offset++] = "0123456789abcdef"[(pid >> shift) & 15];
	}
	name[offset] = 0;

	int64_t listener = nxu_socket_listen(name, 1);

	if (listener < 0) {
		return 3;
	}

	int64_t child = nxu_fork();

	if (child == 0) {
		(void)nxu_close(listener);
		int64_t stream = nxu_socket_connect(name);

		if (stream < 0 || nxu_dup2(stream, 0) != 0 || nxu_dup2(stream, 1) != 1 || nxu_dup2(stream, 2) != 2 ||
			nxu_dup2(2, 2) != 2) {
			return 4;
		}

		(void)nxu_close(stream);
		const char *arguments[] = {"terminaltest", "--child", 0};
		(void)nxu_exec("/Volumes/mainOS/System/Library/CoreServices/terminaltest", arguments);
		return 5;
	}

	if (child < 0) {
		(void)nxu_close(listener);
		return 6;
	}

	int64_t stream = nxu_socket_accept(listener);
	(void)nxu_close(listener);

	if (stream < 0) {
		(void)nxu_kill(child, NXU_SIGKILL);
		return 8;
	}

	uint32_t received = 0;
	uint64_t deadline = nxu_uptime_us() + 10000000;
	bool matched = true;

	for (;;) {
		char block[257];
		int64_t count = nxu_read_poll(stream, block, sizeof(block));

		if (count == -NXU_SYS_E_AGAIN) {
			if ((uint64_t)nxu_uptime_us() >= deadline) {
				matched = false;
				(void)nxu_kill(child, NXU_SIGKILL);
				break;
			}

			(void)nxu_sleep_us(1000);
			continue;
		}

		if (count <= 0) {
			matched = matched && count == 0;
			break;
		}

		for (int64_t index = 0; index < count; index++) {
			char expected = received < TERMINAL_TEST_BYTES		 ? 'x'
							: received < TERMINAL_TEST_BYTES + 7 ? "stderr\n"[received - TERMINAL_TEST_BYTES]
																 : 0;
			matched = matched && block[index] == expected;
			received++;
		}
	}

	(void)nxu_close(stream);
	uint64_t status = 0;
	int64_t waited = nxu_wait(child, &status);

	if (!matched || received != TERMINAL_TEST_BYTES + 7 || waited < 0 || status != 7) {
		netutil_write("terminaltest: FAILED redirected output or child lifetime\n");
		return 9;
	}

	netutil_write("terminaltest: fork/exec, dup2, stdout/stderr, backpressure and EOF passed\n");
	return 0;
}
