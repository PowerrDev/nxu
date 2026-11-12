/* Copyright (c) 2026 NXU Project. All rights reserved. */
/*
 * sigtest: signals on a port that has no fork.
 *
 * proctest covers signals where fork exists; this covers them where a second
 * process can only be spawned. It runs twice over. Started by the kernel it
 * is the parent: it checks handler delivery in its own process, then spawns
 * itself with the argument "child" and checks signals between processes.
 * Started with "child" it is the other half.
 *
 * In one process: a handler runs once, with the right argument, and returns
 * to code whose registers and stack are as they were; the signal is blocked
 * while its handler runs and unblocked after; a blocked signal waits and an
 * ignored one vanishes; SIGKILL and SIGSTOP cannot be caught.
 *
 * Between processes: a signal with a handler interrupts a process that is
 * running; SIGSTOP stops it, wait reports the stop, the process really stands
 * still, SIGCONT sent to its group continues it and wait reports that; a
 * default-fatal signal ends a process and wait reports the signal.
 *
 * The exit status is 0, or the number of the check that failed.
 */
#include <nxu/spawn_io.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define SIGTEST_PATH "/Volumes/mainOS/System/Library/CoreServices/sigtest"
#define SIGTEST_SPINS 200000U

/* What the child exits with once its SIGTERM handler has run. */
#define SIGTEST_CHILD_HANDLED 42ULL

static volatile uint32_t g_hits;
static volatile int g_last_signal;
static volatile uint32_t g_mask_in_handler;
static volatile uint32_t g_terminated;

static void
say(const char *text)
{
	(void)nxu_write(1ULL, text, nxu_strlen(text));
}

static int
fail(int check, const char *what)
{
	char digits[4] = {(char)('0' + (check / 10) % 10), (char)('0' + (check % 10)), ' ', '\0'};

	say("sigtest: FAILED check ");
	say(digits);
	say(what);
	say("\n");
	return check;
}

static void
on_signal(int signal)
{
	g_last_signal = signal;
	g_mask_in_handler = (uint32_t)nxu_sigprocmask(NXU_SIG_BLOCK, 0U);
	g_hits++;
}

static void
on_terminate(int signal)
{
	(void)signal;
	g_terminated = 1U;
}

/*
 * A sum whose intermediate state lives in registers and on the stack across
 * the system call that delivers a signal to this very process: if the handler
 * frame or sigreturn lost any of it, the result is wrong.
 */
static uint64_t
sum_across_signal(void)
{
	uint32_t a = 0x11111111U;
	uint32_t b = 0x22222222U;
	uint32_t c = 0x33333333U;
	uint32_t d = 0x44444444U;
	uint64_t total = 0ULL;

	for (uint32_t round = 0U; round < 8U; round++) {
		a += round;
		b ^= a;
		c += b;
		d ^= c;

		if (round == 4U) (void)nxu_kill((uint64_t)nxu_getpid(), NXU_SIGUSR1);

		total += (uint64_t)a + b + c + d;
	}

	return total;
}

static uint64_t
sum_undisturbed(void)
{
	uint32_t a = 0x11111111U;
	uint32_t b = 0x22222222U;
	uint32_t c = 0x33333333U;
	uint32_t d = 0x44444444U;
	uint64_t total = 0ULL;

	for (uint32_t round = 0U; round < 8U; round++) {
		a += round;
		b ^= a;
		c += b;
		d ^= c;
		total += (uint64_t)a + b + c + d;
	}

	return total;
}

/* The other half: announce readiness on stdout, then run until SIGTERM's handler says stop. */
static int
child_main(void)
{
	if (nxu_signal(NXU_SIGTERM, on_terminate) != 0) return 1;

	(void)nxu_write(1ULL, "r", 1ULL);

	while (g_terminated == 0U)
		(void)nxu_yield();

	return (int)SIGTEST_CHILD_HANDLED;
}

/* Spawn this program as a child that announces itself; its pid, with its stdout stream in *stream. */
static int64_t
spawn_child(const char *mode, int64_t *stream)
{
	const char *arguments[] = {"sigtest", mode, 0};

	return nxu_spawn_io(SIGTEST_PATH, arguments, stream);
}

static int
parent_main(void)
{
	uint64_t status = 0ULL;

	/* One process. */
	if (nxu_signal(NXU_SIGUSR1, on_signal) != 0) return fail(1, "could not install a handler");
	if (nxu_signal(NXU_SIGKILL, on_signal) >= 0) return fail(2, "a SIGKILL handler was accepted");
	if (nxu_signal(NXU_SIGSTOP, on_signal) >= 0) return fail(3, "a SIGSTOP handler was accepted");

	uint64_t expected = sum_undisturbed();
	uint64_t got = sum_across_signal();

	if (g_hits != 1U) return fail(4, "the handler did not run exactly once");
	if (g_last_signal != (int)NXU_SIGUSR1) return fail(5, "the handler got the wrong signal number");
	if (got != expected) return fail(6, "state was not restored after the handler returned");
	if ((g_mask_in_handler & (1U << NXU_SIGUSR1)) == 0U) return fail(7, "the signal was not blocked in its handler");
	if (((uint32_t)nxu_sigprocmask(NXU_SIG_BLOCK, 0U) & (1U << NXU_SIGUSR1)) != 0U)
		return fail(8, "the signal stayed blocked after its handler");

	(void)nxu_sigprocmask(NXU_SIG_BLOCK, 1U << NXU_SIGUSR1);
	if (nxu_kill((uint64_t)nxu_getpid(), NXU_SIGUSR1) != 0) return fail(9, "kill(self) failed");
	(void)nxu_yield();
	if (g_hits != 1U) return fail(10, "a blocked signal was delivered");

	(void)nxu_sigprocmask(NXU_SIG_UNBLOCK, 1U << NXU_SIGUSR1);
	if (g_hits != 2U) return fail(11, "an unblocked signal was not delivered");

	if (nxu_signal(NXU_SIGUSR2, (void (*)(int))(uintptr_t)NXU_SIG_IGN) != 0) return fail(12, "could not ignore");
	if (nxu_kill((uint64_t)nxu_getpid(), NXU_SIGUSR2) != 0) return fail(13, "kill(self, ignored) failed");
	(void)nxu_yield();

	/* Two processes. */
	int64_t stream = -1;
	int64_t child = spawn_child("child", &stream);
	char ready = 0;

	if (child <= 0) return fail(20, "could not spawn the child");
	if (nxu_read((uint64_t)stream, &ready, 1ULL) != 1 || ready != 'r') return fail(21, "the child never got ready");

	/* A job of its own, so the stop below is collected from a group we can also signal. */
	if (nxu_setpgid((uint64_t)child, (uint64_t)child) != 0) return fail(22, "could not give the child a group");

	if (nxu_kill((uint64_t)child, NXU_SIGSTOP) != 0) return fail(23, "kill(child, SIGSTOP) failed");
	if (nxu_wait_options((uint64_t)child, &status, NXU_WAIT_UNTRACED) != child) return fail(24, "stop not reported");
	if (!NXU_WAIT_WAS_STOPPED(status) || NXU_WAIT_STOPSIG(status) != NXU_SIGSTOP)
		return fail(25, "status does not record the stop");
	if (nxu_wait_options((uint64_t)child, &status, NXU_WAIT_UNTRACED | NXU_WAIT_NOHANG) != -NXU_SYS_E_AGAIN)
		return fail(26, "a stop was reported twice");

	/* A handler does not run in a stopped process: the signal waits for the continue. */
	if (nxu_kill((uint64_t)child, NXU_SIGTERM) != 0) return fail(27, "kill(child, SIGTERM) failed");
	for (uint32_t index = 0U; index < 200U; index++)
		(void)nxu_yield();
	if (nxu_wait_options((uint64_t)child, &status, NXU_WAIT_NOHANG) != -NXU_SYS_E_AGAIN)
		return fail(28, "a stopped process acted on a signal");

	if (nxu_killpg((uint64_t)child, NXU_SIGCONT) != 0) return fail(29, "killpg(child's group, SIGCONT) failed");

	/* Continued, it takes the SIGTERM that was waiting: its handler runs and it exits by its own hand. */
	if (nxu_wait((uint64_t)child, &status) != child) return fail(30, "the child was not reaped");
	if (status != SIGTEST_CHILD_HANDLED) return fail(31, "the child's handler did not run after it was continued");

	(void)nxu_close((uint64_t)stream);

	/* A signal whose default action ends a process ends one that has no handler for it. */
	child = spawn_child("child", &stream);
	if (child <= 0) return fail(32, "could not spawn the second child");
	if (nxu_read((uint64_t)stream, &ready, 1ULL) != 1) return fail(33, "the second child never got ready");
	if (nxu_kill((uint64_t)child, NXU_SIGINT) != 0) return fail(34, "kill(child, SIGINT) failed");
	if (nxu_wait((uint64_t)child, &status) != child) return fail(35, "the second child was not reaped");
	if (!NXU_EXIT_WAS_KILLED(status) || NXU_EXIT_TERMSIG(status) != NXU_SIGINT)
		return fail(36, "status does not record SIGINT");

	(void)nxu_close((uint64_t)stream);

	say("sigtest: handlers, masks, stop, continue and kill all worked\n");
	return 0;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc >= 2U && argv != 0 && argv[1] != 0 && nxu_streq(argv[1], "child")) return child_main();

	return parent_main();
}
