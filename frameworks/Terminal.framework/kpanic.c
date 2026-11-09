/* Copyright (c) 2026 NXU Project. All rights reserved. */
/*
 * kpanic -- deliberately panic the kernel, behind a password prompt.
 *
 * A debug/test tool. It asks for a password and, only when it matches, invokes
 * the NXU_SYS_PANIC debug syscall, which stops the machine. The password is a
 * confirmation gate (an "are you sure?"), not a security boundary: the syscall
 * itself is unprivileged, so this guards against a careless keystroke, not a
 * determined caller. Change KPANIC_PASSWORD to taste.
 */
#include "netutil.h"

#include <nxu/string.h>
#include <nxu/syscall.h>

#define KPANIC_PASSWORD "iamsure"
#define KPANIC_INPUT_MAX 64U

/*
 * Read one line from standard input into buffer (always NUL-terminated),
 * stopping at a newline, EOF, or capacity. Returns the length kept. Input is
 * echoed by the terminal, so the password is visible -- acceptable for a debug
 * tool.
 */
static uint32_t
kpanic_read_line(char *buffer, uint32_t capacity)
{
	uint32_t length = 0U;

	while (length + 1U < capacity) {
		char c;
		int64_t n = nxu_read(0, &c, 1);

		if (n <= 0) break; /* EOF or error */
		if (c == '\n' || c == '\r') break;

		buffer[length++] = c;
	}

	buffer[length] = '\0';
	return length;
}

int
main(uint64_t argc, const char *const *argv)
{
	(void)argc;
	(void)argv;

	char input[KPANIC_INPUT_MAX];

	netutil_write("kpanic: this will CRASH the kernel. Password: ");
	(void)kpanic_read_line(input, KPANIC_INPUT_MAX);

	if (!nxu_streq(input, KPANIC_PASSWORD)) {
		netutil_write("\nkpanic: incorrect password; aborting.\n");
		return 1;
	}

	netutil_write("\nkpanic: panicking the kernel now.\n");
	(void)nxu_panic();

	/* Only reached if the syscall somehow returns (it should not). */
	netutil_write("kpanic: the kernel did not panic.\n");
	return 1;
}
