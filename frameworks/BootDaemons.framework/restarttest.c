/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/BootDaemons.framework/restarttest.c
 *
 * restarttest: system calls interrupted by signals, restarted or not.
 *
 * The kernel side (kern/tests/termios_process_test.c) puts a loopback terminal
 * at /dev/ttyrestart, on which what is written comes back as input, and runs
 * this program as an ordinary user. Started with no arguments it runs the
 * checks in a child of its own ("run") and passes on its status: started by
 * the kernel it may be PID 1, which nothing may stop, and the checks need a
 * process that can be. The "run" process makes a system call that sleeps and
 * spawns a child (itself again, as "poke <what>") that interrupts it on a
 * schedule, then checks what the call returned. The child's standard input is the terminal, so it can "type" the
 * byte the parent is waiting for, or restart the terminal's output.
 *
 * Covered, each through a real blocking call:
 *
 *   - a read interrupted by a handler with SA_RESTART is made again and gets
 *     the byte that arrives later; without SA_RESTART it fails with
 *     "interrupted" and the handler has run;
 *   - a read interrupted by a stop and a continue (no handler) is made again
 *     whatever the flags say;
 *   - a write that had moved bytes when it was interrupted returns the short
 *     count, with and without SA_RESTART, and nothing is written twice;
 *   - a write that had moved nothing is made again under SA_RESTART and
 *     completes once output resumes;
 *   - sleep, which is not restartable, fails with "interrupted" even under
 *     SA_RESTART;
 *   - sigaction refuses flags it does not know.
 *
 * The exit status is 0, or the number of the check that failed.
 */
#include <termios.h>

#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define RESTARTTEST_PATH "/Volumes/mainOS/System/Library/CoreServices/restarttest"
#define RESTARTTEST_DEVICE "/dev/ttyrestart"

/* How long the child waits before each step: long enough for the parent to be asleep in its call. */
#define RESTARTTEST_STEP_US 150000ULL

/* What the terminal's output queue holds (kern/tty/tty.h TTY_OUTPUT_QUEUE_SIZE). */
#define RESTARTTEST_QUEUE 4096U

static volatile uint32_t g_handled;

static void
say(const char *text)
{
	(void)nxu_write(1ULL, text, nxu_strlen(text));
}

static int
fail(int check, const char *what)
{
	char digits[4] = {(char)('0' + (check / 10) % 10), (char)('0' + (check % 10)), ' ', '\0'};

	say("restarttest: FAILED check ");
	say(digits);
	say(what);
	say("\n");
	return check;
}

static void
on_signal(int signal)
{
	(void)signal;
	g_handled++;
}

/* Catch SIGUSR1, with or without SA_RESTART. */
static bool
catch_usr1(bool restart)
{
	nxu_sigaction_t action = {.handler = (uint64_t)(uintptr_t)on_signal,
							  .restorer = (uint64_t)(uintptr_t)nxu_sigreturn_trampoline,
							  .mask = 0U,
							  .flags = restart ? NXU_SA_RESTART : 0U};

	g_handled = 0U;
	return nxu_sigaction(NXU_SIGUSR1, &action, 0) == 0;
}

/* ---- the child ---------------------------------------------------------- */

/*
 * poke <what>: interrupt the parent, then (for the modes that end in a byte)
 * type one byte at the terminal, which is standard input.
 *
 *   signal        SIGUSR1, then the byte
 *   signal-only   SIGUSR1
 *   stop          SIGSTOP, SIGCONT, then the byte
 *   signal-start  SIGUSR1, then restart the terminal's stopped output
 */
static int
poke(const char *what)
{
	uint64_t parent = (uint64_t)nxu_getppid();

	(void)nxu_sleep_us(RESTARTTEST_STEP_US);

	if (nxu_streq(what, "stop")) {
		if (nxu_kill(parent, NXU_SIGSTOP) != 0) return 2;

		(void)nxu_sleep_us(RESTARTTEST_STEP_US);

		if (nxu_kill(parent, NXU_SIGCONT) != 0) return 3;
	} else if (nxu_kill(parent, NXU_SIGUSR1) != 0) {
		return 4;
	}

	(void)nxu_sleep_us(RESTARTTEST_STEP_US);

	if (nxu_streq(what, "signal") || nxu_streq(what, "stop")) return nxu_write(0ULL, "x", 1U) == 1 ? 0 : 5;

	if (nxu_streq(what, "signal-start")) return tcflow(0, TCOON) == 0 ? 0 : 6;

	return 0;
}

/* Spawn `poke <what>` with the terminal as its standard input. */
static int64_t
spawn_poker(const char *what, int terminal)
{
	const char *arguments[] = {"restarttest", "poke", what, 0};
	char strings[128];
	nxu_spawn_attr_t attr = {.size = sizeof(attr)};

	if (!nxu_spawn_pack_arguments(arguments, strings, sizeof(strings), &attr.argc, &attr.arguments_size)) return -1;

	attr.arguments = (uint64_t)(uintptr_t)strings;
	attr.name = (uint64_t)(uintptr_t)"restarttest";
	attr.descriptors[attr.descriptor_count++] = (nxu_spawn_fd_t){.child = 0U, .parent = (uint32_t)terminal};

	return nxu_spawn_attr(RESTARTTEST_PATH, &attr);
}

static bool
poker_done(int64_t pid)
{
	uint64_t status = 1U;

	return pid > 0 && nxu_wait((uint64_t)pid, &status) == pid && status == 0U;
}

/* ---- the parent --------------------------------------------------------- */

static uint32_t
queued(int descriptor, uint32_t command)
{
	uint32_t count = UINT32_MAX;

	(void)nxu_ioctl((uint64_t)descriptor, command, &count);
	return count;
}

/* Take everything that came back through the loop; true when it was exactly bytes 0 to count-1 of the pattern. */
static bool
drain_pattern(int quiet, uint32_t count)
{
	uint8_t buffer[512];
	uint32_t got = 0U;

	for (;;) {
		int64_t n = nxu_read((uint64_t)quiet, buffer, sizeof(buffer));

		if (n <= 0) break;

		for (int64_t index = 0; index < n; index++) {
			if (buffer[index] != (uint8_t)((got + (uint32_t)index) * 7U)) return false;
		}

		got += (uint32_t)n;
	}

	return got == count;
}

static uint8_t g_pattern[RESTARTTEST_QUEUE + 1904U];

/* Spawn ourselves as "run" and give back its exit status. */
static int
run_in_child(void)
{
	const char *arguments[] = {"restarttest", "run", 0};
	char strings[64];
	nxu_spawn_attr_t attr = {.size = sizeof(attr)};
	uint64_t status = 0U;

	if (!nxu_spawn_pack_arguments(arguments, strings, sizeof(strings), &attr.argc, &attr.arguments_size)) return 98;

	attr.arguments = (uint64_t)(uintptr_t)strings;
	attr.name = (uint64_t)(uintptr_t)"restarttest";

	int64_t pid = nxu_spawn_attr(RESTARTTEST_PATH, &attr);

	if (pid <= 0 || nxu_wait((uint64_t)pid, &status) != pid) return fail(97, "cannot run the checks in a child");

	return (int)status;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc >= 3U && nxu_streq(argv[1], "poke")) return poke(argv[2]);

	if (argc < 2U || !nxu_streq(argv[1], "run")) return run_in_child();

	int fd = (int)nxu_open(RESTARTTEST_DEVICE, NXU_O_READ | NXU_O_WRITE);
	int quiet = (int)nxu_open(RESTARTTEST_DEVICE, NXU_O_READ | NXU_O_WRITE | NXU_O_NONBLOCK);
	struct termios raw;
	char byte = 0;

	if (fd < 0 || quiet < 0) return fail(1, "cannot open " RESTARTTEST_DEVICE);
	if (tcgetattr(fd, &raw) != 0) return fail(2, "tcgetattr");

	cfmakeraw(&raw);
	raw.c_lflag &= ~ECHO;
	if (tcsetattr(fd, TCSANOW, &raw) != 0) return fail(3, "raw mode");

	nxu_sigaction_t bad = {.handler = NXU_SIG_DFL, .flags = 0x80U};

	if (nxu_sigaction(NXU_SIGUSR1, &bad, 0) != -NXU_SYS_E_INVALID_ARGUMENT) return fail(4, "an unknown sa_flags bit was taken");

	/* A read, SA_RESTART: the handler runs, the read is made again and gets the byte. */
	if (!catch_usr1(true)) return fail(10, "sigaction SA_RESTART");

	int64_t pid = spawn_poker("signal", fd);
	int64_t got = nxu_read((uint64_t)fd, &byte, 1U);

	if (got != 1 || byte != 'x') return fail(11, "a read was not restarted after an SA_RESTART handler");
	if (g_handled != 1U) return fail(12, "the handler did not run exactly once");
	if (!poker_done(pid)) return fail(13, "poker (signal)");

	/* A read, no SA_RESTART: "interrupted", then the byte is still there to be read. */
	if (!catch_usr1(false)) return fail(20, "sigaction without SA_RESTART");

	pid = spawn_poker("signal", fd);
	got = nxu_read((uint64_t)fd, &byte, 1U);

	if (got != -NXU_SYS_E_INTERRUPTED) return fail(21, "a read was restarted without SA_RESTART");
	if (g_handled != 1U) return fail(22, "the handler did not run");
	if (nxu_read((uint64_t)fd, &byte, 1U) != 1 || byte != 'x') return fail(23, "the byte after the interruption");
	if (!poker_done(pid)) return fail(24, "poker (signal)");

	/* Stopped and continued in a read, no handler: made again, whatever the flags. */
	pid = spawn_poker("stop", fd);
	got = nxu_read((uint64_t)fd, &byte, 1U);

	if (got != 1 || byte != 'x') return fail(30, "a read was not restarted after a stop and a continue");
	if (!poker_done(pid)) return fail(31, "poker (stop)");

	/* A write that had moved bytes: the short count, with SA_RESTART and without; nothing written twice. */
	for (uint32_t index = 0U; index < sizeof(g_pattern); index++)
		g_pattern[index] = (uint8_t)(index * 7U);

	for (int pass = 0; pass < 2; pass++) {
		int base = 40 + pass * 5;

		if (!catch_usr1(pass == 0)) return fail(base, "sigaction");
		if (tcflow(fd, TCOOFF) != 0) return fail(base + 1, "tcflow TCOOFF");

		pid = spawn_poker("signal-only", fd);
		got = nxu_write((uint64_t)fd, g_pattern, sizeof(g_pattern));

		if (got != (int64_t)RESTARTTEST_QUEUE || g_handled != 1U)
			return fail(base + 2, "a partial write did not return its short count");
		if (queued(fd, NXU_TIOCOUTQ) != RESTARTTEST_QUEUE) return fail(base + 3, "a partial write was replayed");
		if (tcflow(fd, TCOON) != 0 || !drain_pattern(quiet, RESTARTTEST_QUEUE) || !poker_done(pid))
			return fail(base + 4, "the partial write's bytes did not come back exactly once");
	}

	/* A write that had moved nothing, SA_RESTART: made again, and done once output resumes. */
	if (!catch_usr1(true) || tcflow(fd, TCOOFF) != 0) return fail(60, "setup");
	if (nxu_write((uint64_t)quiet, g_pattern, RESTARTTEST_QUEUE) != (int64_t)RESTARTTEST_QUEUE)
		return fail(61, "fill the output queue");

	pid = spawn_poker("signal-start", fd);
	got = nxu_write((uint64_t)fd, g_pattern + RESTARTTEST_QUEUE, 16U);

	if (got != 16 || g_handled != 1U) return fail(62, "a write with nothing written was not restarted");
	if (!poker_done(pid)) return fail(63, "poker (signal-start)");
	if (!drain_pattern(quiet, RESTARTTEST_QUEUE + 16U)) return fail(64, "the restarted write's bytes");

	/* Sleep is not restartable: "interrupted" even under SA_RESTART. */
	if (!catch_usr1(true)) return fail(70, "sigaction");

	uint64_t before = (uint64_t)nxu_uptime_us();

	pid = spawn_poker("signal-only", fd);
	got = nxu_sleep_us(3000000ULL);

	if (got != -NXU_SYS_E_INTERRUPTED || g_handled != 1U) return fail(71, "a sleep was restarted, or not interrupted");
	if ((uint64_t)nxu_uptime_us() - before >= 3000000ULL) return fail(72, "the sleep ran its whole length");
	if (!poker_done(pid)) return fail(73, "poker (signal-only)");

	(void)nxu_close((uint64_t)quiet);
	(void)nxu_close((uint64_t)fd);

	say("restarttest: interrupted system calls were restarted, or not, as they should be\n");
	return 0;
}
