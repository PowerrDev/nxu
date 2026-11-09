/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * suidtest -- demonstrate setuid-exec (Phase 4, doc/architecture/unix-security.md).
 * As root it marks suidhelper setuid-root (chmod 04755), drops itself to a
 * non-root UID, then spawns the helper. The kernel's loader sees the setuid bit
 * and gives the child the file owner's (root's) effective UID; the helper
 * reports that back through its exit code. The kernel also logs the transition
 * ("loader: setuid-exec ... euid 1000 -> 0") to the console.
 *
 * i386 has no fork, so this uses spawn; dropping privilege is one-way, which is
 * fine (the helper is a dedicated test binary left setuid-root).
 */
#include <nxu/syscall.h>

#include <stdint.h>

#define SUIDHELPER_PATH "/Volumes/mainOS/System/Library/CoreServices/suidhelper"
#define SUIDTEST_UID 1000U

static void
say(const char *text)
{
	uint64_t length = 0ULL;
	while (text[length] != '\0')
		length++;
	(void)nxu_write(1ULL, text, length);
}

static void
say_u32(uint32_t value)
{
	char digits[11];
	int index = 10;
	digits[index] = '\0';
	if (value == 0U) {
		say("0");
		return;
	}
	while (value != 0U) {
		digits[--index] = (char)('0' + (value % 10U));
		value /= 10U;
	}
	say(&digits[index]);
}

int
main(void)
{
	say("suidtest: running as uid ");
	say_u32((uint32_t)nxu_geteuid());
	say("\n");

	if (nxu_chmod(SUIDHELPER_PATH, 04755U) < 0) {
		say("suidtest: chmod 04755 " SUIDHELPER_PATH " failed (are we root?)\n");
		return 1;
	}
	say("suidtest: marked suidhelper setuid-root (04755)\n");

	if (nxu_setuid(SUIDTEST_UID) < 0) {
		say("suidtest: setuid failed\n");
		return 1;
	}
	say("suidtest: dropped to uid ");
	say_u32((uint32_t)nxu_geteuid());
	say("\n");

	int64_t child = nxu_spawn(SUIDHELPER_PATH, "suidhelper");
	if (child < 0) {
		say("suidtest: spawn failed\n");
		return 1;
	}

	uint64_t status = 0ULL;
	(void)nxu_waitpid((uint64_t)child, &status);
	if (NXU_EXIT_WAS_KILLED(status)) {
		say("  helper crashed\n");
		return 1;
	}

	uint32_t code = (uint32_t)(status & 0xFFULL);
	if (code == 7U)
		say("  helper ran with euid 0 -> setuid-exec ELEVATED it to root!\n"
			"  (the kernel logged 'loader: setuid-exec ... euid 1000 -> 0')\n");
	else if (code == 8U)
		say("  helper ran as non-root -> setuid-exec did NOT apply (unexpected)\n");
	else {
		say("  helper exited with unexpected code ");
		say_u32(code);
		say("\n");
	}
	return 0;
}
