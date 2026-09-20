#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * The positive half of the capability tests (the negative half, an ordinary
 * process being refused, is in proctest.c). process_control_test starts this
 * program from the kernel holding FS_WRITE and DISPLAY, and it works out what
 * to do from the set it finds itself with:
 *
 *   FS_WRITE|DISPLAY   the parent: uses what it was given, is refused what it
 *                      was not, and starts children with subsets
 *   FS_WRITE           a child given a subset: still writes, no display
 *   none               a child given nothing: refused everything
 *
 * Exit status 0 is success; any other value is the number of the check that
 * failed.
 */

#define PRIVTEST_PATH "/disk/System/Library/CoreServices/privtest"
#define PRIVTEST_FILE "/disk/var/privtest.tmp"
#define PRIVTEST_DIR "/disk/var/privtest.d"

#define PRIVTEST_PARENT_CAPS (NXU_CAP_FS_WRITE | NXU_CAP_DISPLAY)

static void
say(const char *text)
{
	uint64_t length = 0ULL;
	while (text[length] != '\0') length++;
	(void)nxu_write(1ULL, text, length);
}

static int
fail(int check, const char *what)
{
	say("privtest: FAILED check ");
	char digits[4] = { (char)('0' + (check / 10)), (char)('0' + (check % 10)), ' ', '\0' };
	say(digits);
	say(what);
	say("\n");
	return check;
}

/* Create, write, close and remove a file, and make a directory: what FS_WRITE allows. */
static int
use_filesystem(int base)
{
	int64_t descriptor = nxu_open(PRIVTEST_FILE, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (descriptor < 0) return fail(base, "open for write was refused despite FS_WRITE");
	if (nxu_write((uint64_t)descriptor, "x", 1ULL) != 1) return fail(base + 1, "write failed");
	(void)nxu_close((uint64_t)descriptor);
	if (nxu_unlink(PRIVTEST_FILE) != 0) return fail(base + 2, "unlink was refused despite FS_WRITE");

	/* The directory may be left over from an earlier run on a persistent disk. */
	int64_t made = nxu_mkdir(PRIVTEST_DIR);
	if (made != 0 && made != -NXU_SYS_E_EXISTS) return fail(base + 3, "mkdir was refused despite FS_WRITE");
	(void)nxu_unlink(PRIVTEST_DIR);
	return 0;
}

/* Everything a process with no capabilities must be refused. */
static int
refused_everything(int base)
{
	if (nxu_open(PRIVTEST_FILE, NXU_O_WRITE | NXU_O_CREATE) != -NXU_SYS_E_DENIED) return fail(base, "open(write|create) was not refused");
	if (nxu_open(PRIVTEST_FILE, NXU_O_APPEND) != -NXU_SYS_E_DENIED) return fail(base + 1, "open(append) was not refused");
	if (nxu_unlink(PRIVTEST_FILE) != -NXU_SYS_E_DENIED) return fail(base + 2, "unlink was not refused");
	if (nxu_mkdir(PRIVTEST_DIR) != -NXU_SYS_E_DENIED) return fail(base + 3, "mkdir was not refused");
	if (nxu_display_claim() != -NXU_SYS_E_DENIED) return fail(base + 4, "display_claim was not refused");
	if (nxu_system_reset() != -NXU_SYS_E_DENIED) return fail(base + 5, "system_reset was not refused");
	return 0;
}

static int
child_with_nothing(void)
{
	if (nxu_get_caps() != 0) return fail(20, "a child started with none holds capabilities");
	return refused_everything(21);
}

static int
child_with_filesystem(void)
{
	if (nxu_get_caps() != (int64_t)NXU_CAP_FS_WRITE) return fail(30, "the subset FS_WRITE was not passed on exactly");
	int result = use_filesystem(31);
	if (result != 0) return result;
	if (nxu_display_claim() != -NXU_SYS_E_DENIED) return fail(35, "the child was given the display its parent held");
	return 0;
}

int
main(int argc, char **argv)
{
	(void)argv;

	int64_t caps = nxu_get_caps();
	if (argc == 2) return caps == (int64_t)PRIVTEST_PARENT_CAPS ? 0 : fail(40, "exec changed the capability set");
	if (caps == 0) return child_with_nothing();
	if (caps == (int64_t)NXU_CAP_FS_WRITE) return child_with_filesystem();
	if (caps != (int64_t)PRIVTEST_PARENT_CAPS) return fail(1, "started with a set other than FS_WRITE|DISPLAY");

	uint64_t status = 0ULL;

	/* What was granted works... */
	int result = use_filesystem(2);
	if (result != 0) return result;
	if (nxu_display_claim() != 0) return fail(6, "display_claim was refused despite DISPLAY");

	/* ...and what was not is refused, even though PID 1's set is larger. */
	if (nxu_system_reset() != -NXU_SYS_E_DENIED) return fail(7, "system_reset was not refused without RESET");

	/* A child can be given no more than its parent holds. */
	if (nxu_spawn_caps(PRIVTEST_PATH, "privtest", NXU_CAP_RESET) != -NXU_SYS_E_DENIED) return fail(8, "spawn passed on a capability the parent lacks");
	if (nxu_spawn_caps(PRIVTEST_PATH, "privtest", NXU_CAP_FS_WRITE | NXU_CAP_RESET) != -NXU_SYS_E_DENIED) return fail(9, "spawn passed on a set larger than the parent's");
	if (nxu_spawn_caps(PRIVTEST_PATH, "privtest", 0x80ULL) != -NXU_SYS_E_INVALID_ARGUMENT) return fail(10, "spawn accepted a bit that is not a capability");

	/* fork keeps the caller's set. */
	int64_t pid = nxu_fork();
	if (pid < 0) return fail(11, "fork failed");
	if (pid == 0) {
		int ok = nxu_get_caps() == (int64_t)PRIVTEST_PARENT_CAPS && nxu_system_reset() == -NXU_SYS_E_DENIED;
		(void)nxu_exit(ok ? 0ULL : 1ULL);
	}
	if (nxu_wait((uint64_t)pid, &status) != pid || status != 0ULL) return fail(12, "the forked child's set differed from its parent's");

	/* exec keeps it too. */
	pid = nxu_fork();
	if (pid < 0) return fail(13, "fork failed");
	if (pid == 0) {
		const char *const exec_argv[] = { "privtest", "exec", 0 };
		(void)nxu_exec(PRIVTEST_PATH, exec_argv);
		(void)nxu_exit(99ULL);
	}
	if (nxu_wait((uint64_t)pid, &status) != pid || status != 0ULL) return fail(14, "exec changed the capability set");

	/* A spawned child gets exactly what was passed: nothing, then a subset. */
	pid = nxu_spawn_caps(PRIVTEST_PATH, "privtest", 0ULL);
	if (pid <= 0) return fail(15, "spawn with no capabilities failed");
	if (nxu_wait((uint64_t)pid, &status) != pid || status != 0ULL) return fail(16, "the child started with none was not refused everything");

	pid = nxu_spawn_caps(PRIVTEST_PATH, "privtest", NXU_CAP_FS_WRITE);
	if (pid <= 0) return fail(17, "spawn with FS_WRITE failed");
	if (nxu_wait((uint64_t)pid, &status) != pid || status != 0ULL) return fail(18, "the child given a subset did not get exactly that");

	return 0;
}
