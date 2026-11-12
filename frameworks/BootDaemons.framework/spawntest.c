/* Copyright (c) 2026 NXU Project. All rights reserved. */
/*
 * spawntest: what a spawned process starts with.
 *
 * Run without arguments it is the parent: it spawns itself in various ways
 * and checks, from both sides, that the child had everything it was meant to
 * have from its first instruction. Run with a mode as argv[1] it is one of
 * the children, which checks its own half and says what it found with its
 * exit status (0 for "as expected").
 *
 * Covered: arguments arrive intact, an empty one included; only the
 * descriptors that were named are inherited, at the numbers that were asked
 * for; the child is in its parent's group by default, in a new group, in an
 * existing group, or at the head of a new session, and is there before the
 * spawn call has returned; the signal mask it starts with; and the requests
 * that must be refused are, leaving no child behind.
 *
 * The exit status of the parent is 0, or the number of the check that failed.
 */
#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define SPAWNTEST_PATH "/Volumes/mainOS/System/Library/CoreServices/spawntest"

/* A pid no process has, and so a group that does not exist. */
#define SPAWNTEST_NO_SUCH_PID 99999U

/* The descriptor number the "fd" child is given its one inherited file at. */
#define SPAWNTEST_CHILD_FD 5U

static void
say(const char *text)
{
	(void)nxu_write(1ULL, text, nxu_strlen(text));
}

static int
fail(int check, const char *what)
{
	char digits[4] = {(char)('0' + (check / 10) % 10), (char)('0' + (check % 10)), ' ', '\0'};

	say("spawntest: FAILED check ");
	say(digits);
	say(what);
	say("\n");
	return check;
}

static void
format_decimal(uint64_t value, char text[24])
{
	char reversed[24];
	uint32_t length = 0;

	do {
		reversed[length++] = (char)('0' + value % 10);
		value /= 10;
	} while (value != 0);

	for (uint32_t index = 0; index < length; index++)
		text[index] = reversed[length - 1 - index];

	text[length] = '\0';
}

static uint64_t
parse_decimal(const char *text)
{
	uint64_t value = 0;

	for (; *text >= '0' && *text <= '9'; text++)
		value = value * 10 + (uint64_t)(*text - '0');

	return value;
}

/* "args": the exact argument list the parent sent. */
static int
child_args(uint64_t argc, const char *const *argv)
{
	if (argc != 6) return 1;
	if (!nxu_streq(argv[0], "spawntest") || !nxu_streq(argv[1], "args")) return 2;
	if (!nxu_streq(argv[2], "alpha")) return 3;
	if (!nxu_streq(argv[3], "two words")) return 4;
	if (argv[4][0] != '\0') return 5;
	if (!nxu_streq(argv[5], "last")) return 6;
	if (argv[6] != 0) return 7;

	return 0;
}

/* "fd": one inherited descriptor, at the number asked for, and nothing else. */
static int
child_fd(void)
{
	if (nxu_write(SPAWNTEST_CHILD_FD, "ok", 2ULL) != 2) return 1;

	for (uint64_t descriptor = 3; descriptor < 16; descriptor++) {
		if (descriptor == SPAWNTEST_CHILD_FD) continue;

		if (nxu_write(descriptor, "x", 1ULL) != -NXU_SYS_E_BAD_FD) return 2;
	}

	char byte;

	/* Nothing was given as standard input. */
	if (nxu_read(0ULL, &byte, 1ULL) != -NXU_SYS_E_BAD_FD) return 3;

	return 0;
}

/* "ids <pgid> <sid>": our group and session; 0 stands for "named after us". */
static int
child_ids(uint64_t argc, const char *const *argv)
{
	if (argc != 4) return 1;

	uint64_t self = (uint64_t)nxu_getpid();
	uint64_t pgid = parse_decimal(argv[2]);
	uint64_t sid = parse_decimal(argv[3]);

	if ((uint64_t)nxu_getpgid(0) != (pgid == 0 ? self : pgid)) return 2;
	if ((uint64_t)nxu_getsid(0) != (sid == 0 ? self : sid)) return 3;

	return 0;
}

/* "mask <mask>": the signal mask we started with. */
static int
child_mask(uint64_t argc, const char *const *argv)
{
	if (argc != 3) return 1;

	return (uint64_t)nxu_sigprocmask(NXU_SIG_BLOCK, 0U) == parse_decimal(argv[2]) ? 0 : 2;
}

/* "wait": stay alive until killed, so a group named after us exists. */
static int
child_wait(void)
{
	for (;;)
		(void)nxu_sleep_us(10000ULL);
}

/* Spawn ourselves with `arguments` and `attr` (whose argument fields are filled in here). */
static int64_t
spawn_self(const char *const *arguments, nxu_spawn_attr_t *attr)
{
	static char strings[NXU_EXEC_ARGV_BYTES];

	attr->size = sizeof(*attr);

	if (!nxu_spawn_pack_arguments(arguments, strings, sizeof(strings), &attr->argc, &attr->arguments_size))
		return -NXU_SYS_E_INVALID_ARGUMENT;

	attr->arguments = (uint64_t)(uintptr_t)strings;

	return nxu_spawn_attr(SPAWNTEST_PATH, attr);
}

/* Spawn ourselves and return the child's exit status, or UINT64_MAX if it could not be spawned or reaped. */
static uint64_t
run_child(const char *const *arguments, nxu_spawn_attr_t *attr)
{
	uint64_t status = UINT64_MAX;
	int64_t child = spawn_self(arguments, attr);

	if (child <= 0 || nxu_wait((uint64_t)child, &status) != child) return UINT64_MAX;

	return status;
}

static int
parent_main(void)
{
	uint64_t self = (uint64_t)nxu_getpid();
	uint64_t our_pgid = (uint64_t)nxu_getpgid(0);
	uint64_t our_sid = (uint64_t)nxu_getsid(0);
	char pgid_text[24];
	char sid_text[24];
	uint64_t status = 0;

	(void)self;

	/* Arguments. */
	{
		const char *const arguments[] = {"spawntest", "args", "alpha", "two words", "", "last", 0};
		nxu_spawn_attr_t attr = {0};

		status = run_child(arguments, &attr);
		if (status != 0ULL) return fail(1, "arguments did not arrive intact (the child's status says which)");
	}

	/* Descriptors: exactly the one that was named, where it was asked for. */
	{
		const char *const arguments[] = {"spawntest", "fd", 0};
		uint32_t ends[2];

		if (nxu_socketpair(ends) != 0) return fail(2, "socketpair failed");

		nxu_spawn_attr_t attr = {.descriptor_count = 1, .descriptors = {{.child = SPAWNTEST_CHILD_FD, .parent = ends[1]}}};
		int64_t child = spawn_self(arguments, &attr);

		if (child <= 0) return fail(3, "could not spawn with a descriptor");

		(void)nxu_close(ends[1]);

		char bytes[2] = {0, 0};

		if (nxu_read(ends[0], bytes, 2ULL) != 2 || bytes[0] != 'o' || bytes[1] != 'k')
			return fail(4, "the child could not use its inherited descriptor");
		if (nxu_wait((uint64_t)child, &status) != child || status != 0ULL)
			return fail(5, "the child had descriptors it was not given, or lacked the one it was");

		(void)nxu_close(ends[0]);

		/* A descriptor the parent does not have cannot be inherited, and no child results. */
		nxu_spawn_attr_t bad = {.descriptor_count = 1, .descriptors = {{.child = 0, .parent = 77}}};

		if (spawn_self(arguments, &bad) != -NXU_SYS_E_BAD_FD) return fail(6, "inheriting a closed descriptor was allowed");
		if (nxu_wait_options(NXU_WAIT_ANY, &status, NXU_WAIT_NOHANG) != -NXU_SYS_E_NOT_FOUND)
			return fail(7, "a refused spawn left a child behind");
	}

	/* Process groups and sessions. */
	{
		format_decimal(our_pgid, pgid_text);
		format_decimal(our_sid, sid_text);

		/* By default: the parent's group and session. */
		const char *const inherit[] = {"spawntest", "ids", pgid_text, sid_text, 0};
		nxu_spawn_attr_t attr = {0};

		if (run_child(inherit, &attr) != 0ULL) return fail(10, "a child did not start in its parent's group");

		/* A new group, named after the child, in the parent's session. */
		const char *const own_group[] = {"spawntest", "ids", "0", sid_text, 0};
		nxu_spawn_attr_t new_group = {.flags = NXU_SPAWN_SETPGROUP};

		if (run_child(own_group, &new_group) != 0ULL) return fail(11, "a child did not start in a group of its own");

		/* An existing group: `anchor` makes one, the next child is born into it. */
		const char *const waiting[] = {"spawntest", "wait", 0};
		nxu_spawn_attr_t anchor_attr = {.flags = NXU_SPAWN_SETPGROUP};
		int64_t anchor = spawn_self(waiting, &anchor_attr);

		if (anchor <= 0) return fail(12, "could not spawn the group's first member");

		/* True the moment spawn returns, whether or not the child has run yet. */
		if (nxu_getpgid((uint64_t)anchor) != anchor) return fail(13, "the child was not in its group when spawn returned");

		char anchor_text[24];

		format_decimal((uint64_t)anchor, anchor_text);

		const char *const join[] = {"spawntest", "ids", anchor_text, sid_text, 0};
		nxu_spawn_attr_t join_attr = {.flags = NXU_SPAWN_SETPGROUP, .pgid = (uint32_t)anchor};

		if (run_child(join, &join_attr) != 0ULL) return fail(14, "a child did not start in the group it was to join");

		if (nxu_kill((uint64_t)anchor, NXU_SIGKILL) != 0 || nxu_wait((uint64_t)anchor, &status) != anchor)
			return fail(15, "could not end the group's first member");

		/* A new session. */
		const char *const session[] = {"spawntest", "ids", "0", "0", 0};
		nxu_spawn_attr_t session_attr = {.flags = NXU_SPAWN_SETSID};

		if (run_child(session, &session_attr) != 0ULL) return fail(16, "a child did not start at the head of a session");

		/* A group that does not exist cannot be joined, and no child results. */
		nxu_spawn_attr_t missing = {.flags = NXU_SPAWN_SETPGROUP, .pgid = SPAWNTEST_NO_SUCH_PID};

		if (spawn_self(waiting, &missing) != -NXU_SYS_E_DENIED) return fail(17, "a child joined a group that does not exist");

		nxu_spawn_attr_t both = {.flags = NXU_SPAWN_SETPGROUP | NXU_SPAWN_SETSID};

		if (spawn_self(waiting, &both) != -NXU_SYS_E_INVALID_ARGUMENT)
			return fail(18, "a new session and a chosen group were accepted together");
		if (nxu_wait_options(NXU_WAIT_ANY, &status, NXU_WAIT_NOHANG) != -NXU_SYS_E_NOT_FOUND)
			return fail(19, "a refused spawn left a child behind");

		/* None of it moved us. */
		if ((uint64_t)nxu_getpgid(0) != our_pgid || (uint64_t)nxu_getsid(0) != our_sid)
			return fail(20, "spawning changed the parent's own group or session");
	}

	/* The signal mask. */
	{
		char mask_text[24];
		uint32_t mask = (1U << NXU_SIGUSR1) | (1U << NXU_SIGTERM);

		format_decimal(mask, mask_text);

		const char *const masked[] = {"spawntest", "mask", mask_text, 0};
		nxu_spawn_attr_t attr = {.flags = NXU_SPAWN_SETSIGMASK, .sigmask = mask};

		if (run_child(masked, &attr) != 0ULL) return fail(30, "a child did not start with the mask it was given");

		const char *const unmasked[] = {"spawntest", "mask", "0", 0};
		nxu_spawn_attr_t plain = {0};

		if (run_child(unmasked, &plain) != 0ULL) return fail(31, "a child started with signals blocked unasked");
	}

	say("spawntest: arguments, descriptors, groups, sessions and masks were all in place at the start\n");
	return 0;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc < 2U || argv == 0 || argv[1] == 0) return parent_main();

	if (nxu_streq(argv[1], "args")) return child_args(argc, argv);
	if (nxu_streq(argv[1], "fd")) return child_fd();
	if (nxu_streq(argv[1], "ids")) return child_ids(argc, argv);
	if (nxu_streq(argv[1], "mask")) return child_mask(argc, argv);
	if (nxu_streq(argv[1], "wait")) return child_wait();

	return 100;
}
