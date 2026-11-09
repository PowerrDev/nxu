/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * credtest -- demonstrate Phase 3 permission enforcement. The shell runs as
 * root, and root bypasses file-mode checks, so this tool tightens a test file
 * to owner-only (0600), forks, drops the child to a non-root UID, and shows the
 * child being denied read access by the mode bits. The root parent restores the
 * original mode afterwards. See doc/architecture/unix-security.md.
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
	uint32_t saved_mode = st.mode & 0x0FFFU;

	if (nxu_chmod(CREDTEST_PATH, 0600U) < 0) {
		say("credtest: chmod 0600 failed (are we root?)\n");
		return 1;
	}
	say("credtest: tightened " CREDTEST_PATH " to 0600 (owner root rw)\n");

	int64_t child = nxu_fork();
	if (child < 0) {
		say("credtest: fork failed\n");
		(void)nxu_chmod(CREDTEST_PATH, saved_mode);
		return 1;
	}

	if (child == 0) {
		if (nxu_setuid(CREDTEST_UID) < 0) {
			say("  child: setuid failed\n");
			return 1;
		}
		say("  child: dropped to uid ");
		say_u32((uint32_t)nxu_geteuid());
		say("\n");

		int64_t fd = nxu_open(CREDTEST_PATH, NXU_O_READ);
		if (fd < 0) {
			say("  child: open for read -> DENIED  <- enforcement works\n");
		} else {
			say("  child: open for read -> ALLOWED (unexpected!)\n");
			(void)nxu_close((uint64_t)fd);
		}
		return 0;
	}

	uint64_t status = 0ULL;
	(void)nxu_waitpid((uint64_t)child, &status);
	(void)nxu_chmod(CREDTEST_PATH, saved_mode);
	say("credtest: restored original mode, done\n");
	return 0;
}
