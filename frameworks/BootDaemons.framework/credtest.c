/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * credtest -- demonstrate Phase 3 permission enforcement. The shell runs as
 * root, and root bypasses every check, so this tool drops itself to a non-root
 * UID (root -> 1000 is always allowed) and then shows the kernel refusing
 * privileged operations on a root-owned file: a non-owner may not chmod it, a
 * non-root process may not chown it, while an ordinary world-readable open
 * still succeeds. Nothing is modified -- the denied operations change nothing.
 * See doc/architecture/unix-security.md.
 *
 * i386 has no fork (MACHINE_USER_CONTEXT 0), so this runs in a single process;
 * dropping privilege is one-way, which is fine because the tool only reads.
 */
#include <nxu/syscall.h>

#include <stdint.h>

#define CREDTEST_PATH "/Volumes/mainOS/System/README.txt"
#define CREDTEST_UID 1000U

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

/* Report an operation that is expected to be refused. */
static void
report(const char *what, int64_t result)
{
	say(what);
	if (result < 0)
		say(" -> DENIED  <- enforcement works\n");
	else
		say(" -> ALLOWED (unexpected!)\n");
}

int
main(void)
{
	say("credtest: running as uid ");
	say_u32((uint32_t)nxu_geteuid());
	say("\n");

	nxu_stat_t st;
	if (nxu_stat(CREDTEST_PATH, &st) < 0) {
		say("credtest: cannot stat " CREDTEST_PATH "\n");
		return 1;
	}
	say("credtest: " CREDTEST_PATH " is owned by uid ");
	say_u32(st.uid);
	say("\n");

	if (nxu_setuid(CREDTEST_UID) < 0) {
		say("credtest: setuid failed\n");
		return 1;
	}
	say("credtest: dropped to uid ");
	say_u32((uint32_t)nxu_geteuid());
	say("\n");

	/* Now non-root. These touch a root-owned file and must be refused. */
	report("  chmod 0600 (not the owner)", nxu_chmod(CREDTEST_PATH, 0600U));
	report("  chown to self (not root)   ", nxu_chown(CREDTEST_PATH, CREDTEST_UID, NXU_ID_UNCHANGED));

	/* A plain read of a world-readable file still works. */
	int64_t fd = nxu_open(CREDTEST_PATH, NXU_O_READ);
	if (fd >= 0) {
		say("  read world-readable file   -> ALLOWED (correct)\n");
		(void)nxu_close((uint64_t)fd);
	} else {
		say("  read world-readable file   -> DENIED (unexpected!)\n");
	}

	say("credtest: done (nothing was modified)\n");
	return 0;
}
