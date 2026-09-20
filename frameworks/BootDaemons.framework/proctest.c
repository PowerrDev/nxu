#include <nxu/syscall.h>
#include <nxu/thread.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Process-control test: spawned directly by kern/tests/process_control_test.c.
 * Exercises, from real user code, everything fork / exec / signals /
 * copy-on-write / demand paging add. main returns 0 when every check passes,
 * otherwise the number of the first check that failed.
 */

#define PROCTEST_WAIT_SPINS 2000000U
#define PROCTEST_MMAP_BYTES (16ULL * 1024ULL * 1024ULL)
#define PROCTEST_EXEC_PATH "/disk/System/Library/CoreServices/execchild"
#define PROCTEST_EXEC_STATUS 31

#define SHARED_CHILD_READY 0U
#define SHARED_PARENT_WROTE 1U
#define SHARED_RESULT 2U
#define SHARED_CHILD_ARMED 3U
#define PROCTEST_THREAD_STACK (16ULL * 1024ULL)

static volatile uint64_t g_value = 111ULL;
static volatile uint64_t g_handler_hits;
static volatile int g_last_signal;
static volatile uint32_t g_mask_inside_handler;
static volatile uint64_t g_child_signals;

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
	say("proctest: FAILED check ");
	char digits[4] = { (char)('0' + (check / 10)), (char)('0' + (check % 10)), ' ', '\0' };
	say(digits);
	say(what);
	say("\n");
	return check;
}

static int64_t
wait_child(int64_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < PROCTEST_WAIT_SPINS; spin++) {
		int64_t result = nxu_waitpid((uint64_t)pid, status);
		if (result != -NXU_SYS_E_AGAIN) return result;
		(void)nxu_yield();
	}

	return -NXU_SYS_E_AGAIN;
}

static bool
wait_flag(volatile uint64_t *flag)
{
	for (uint32_t spin = 0U; spin < PROCTEST_WAIT_SPINS; spin++) {
		if (*flag != 0ULL) return true;
		(void)nxu_yield();
	}

	return false;
}

static void
handler_usr1(int signal)
{
	g_last_signal = signal;
	g_handler_hits++;
	g_mask_inside_handler = (uint32_t)nxu_sigprocmask(NXU_SIG_BLOCK, 0U);
}

static void
handler_chld(int signal)
{
	(void)signal;
	g_child_signals++;
}

static void
handler_segv_exit(int signal)
{
	(void)signal;
	(void)nxu_exit(55ULL);
}

/* Runs on a second thread: lets the main thread go to sleep, then signals the
 * process, which must cut that sleep short. */
static void
signal_self_thread(void *arg)
{
	(void)arg;

	for (uint32_t index = 0U; index < 30U; index++) (void)nxu_yield();

	(void)nxu_kill((uint64_t)nxu_getpid(), NXU_SIGUSR1);
	(void)nxu_thread_exit(0ULL);
}

static void
handler_ignore_marker(int signal)
{
	(void)signal;
}

static uint64_t
deep_stack(uint32_t depth)
{
	volatile uint8_t page[4096];

	page[0] = (uint8_t)depth;
	page[4095] = (uint8_t)(depth + 1U);

	uint64_t inner = depth == 0U ? 0ULL : deep_stack(depth - 1U);

	if (page[0] != (uint8_t)depth || page[4095] != (uint8_t)(depth + 1U)) return UINT64_MAX;
	return inner + 1ULL;
}

int
main(void)
{
	uint64_t status = 0ULL;

	/* 1. fork: the child resumes with 0 and exits; the parent reaps it. */
	{
		int64_t pid = nxu_fork();
		if (pid < 0) return fail(1, "fork failed");
		if (pid == 0) (void)nxu_exit(7ULL);

		if (wait_child(pid, &status) != pid || status != 7ULL) return fail(2, "child exit status not 7");
		if (nxu_getpid() == pid) return fail(3, "child has the parent's pid");
	}

	/* 2. Copy-on-write, both directions, with a shared page that must stay shared. */
	{
		int64_t shared_id = nxu_shm_create(4096ULL);
		if (shared_id <= 0) return fail(4, "shm_create failed");

		volatile uint64_t *shared = (volatile uint64_t *)nxu_shm_map((uint64_t)shared_id);
		if ((int64_t)shared <= 0) return fail(5, "shm_map failed");

		g_value = 1111ULL;

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(6, "fork failed");

		if (pid == 0) {
			/* Child: tell the parent we are up, wait for its write, then check
			 * that we still see the pre-fork value and can diverge ourselves. */
			shared[SHARED_CHILD_READY] = 1ULL;
			if (!wait_flag(&shared[SHARED_PARENT_WROTE])) (void)nxu_exit(1ULL);

			if (g_value != 1111ULL) (void)nxu_exit(2ULL);

			g_value = 2222ULL;
			if (g_value != 2222ULL) (void)nxu_exit(3ULL);

			shared[SHARED_RESULT] = 42ULL;
			(void)nxu_exit(0ULL);
		}

		if (!wait_flag(&shared[SHARED_CHILD_READY])) return fail(7, "child never started");

		g_value = 3333ULL;
		shared[SHARED_PARENT_WROTE] = 1ULL;

		if (wait_child(pid, &status) != pid || status != 0ULL) return fail(8, "child saw the parent's write or failed");
		if (g_value != 3333ULL) return fail(9, "child's write leaked into the parent");
		if (shared[SHARED_RESULT] != 42ULL) return fail(10, "shared memory stopped being shared across fork");
	}

	/* 3. Demand paging: a big region costs nothing until touched, reads are zero. */
	{
		volatile uint8_t *region = (volatile uint8_t *)nxu_mmap(PROCTEST_MMAP_BYTES, NXU_MMAP_PROT_READ_WRITE);
		if ((int64_t)region <= 0) return fail(11, "mmap of 16 MiB failed");

		if (region[4096ULL * 100ULL] != 0U) return fail(12, "untouched page is not zero");

		region[0] = 1U;
		region[5ULL * 1024ULL * 1024ULL] = 2U;
		region[PROCTEST_MMAP_BYTES - 1ULL] = 3U;

		if (region[0] != 1U || region[5ULL * 1024ULL * 1024ULL] != 2U || region[PROCTEST_MMAP_BYTES - 1ULL] != 3U) {
			return fail(13, "touched pages lost their contents");
		}

		if (region[4096ULL * 101ULL] != 0U) return fail(14, "neighbour of a touched page is not zero");

		/* The kernel writing into a page nobody has touched must fault it in too. */
		if (nxu_get_boot_args((char *)(region + 4096ULL * 200ULL), 256ULL) < 0) return fail(15, "kernel write into an untouched page failed");

		if (nxu_munmap((uint64_t)region, PROCTEST_MMAP_BYTES) < 0) return fail(16, "munmap failed");
	}

	/* 4. The lazy stack grows to its full size on demand (128 KiB here). */
	if (deep_stack(31U) != 32ULL) return fail(17, "deep recursion corrupted the stack");

	/* 5. Many forks in a row: exit statuses come back right, resources come back. */
	for (uint32_t index = 0U; index < 8U; index++) {
		int64_t pid = nxu_fork();
		if (pid < 0) return fail(18, "fork failed in the loop");
		if (pid == 0) (void)nxu_exit(100ULL + index);

		if (wait_child(pid, &status) != pid || status != 100ULL + index) return fail(19, "wrong status from a looped child");
	}

	/* 6. exec: argv arrives, the image is replaced, the exit status proves it. */
	{
		int64_t pid = nxu_fork();
		if (pid < 0) return fail(20, "fork failed");

		if (pid == 0) {
			const char *argv[] = { "execchild", "alpha", "beta", 0 };
			(void)nxu_exec(PROCTEST_EXEC_PATH, argv);
			(void)nxu_exit(99ULL);
		}

		if (wait_child(pid, &status) != pid) return fail(21, "exec child never exited");
		if (status == 99ULL) return fail(22, "exec returned");
		if (status != (uint64_t)PROCTEST_EXEC_STATUS) return fail(23, "exec'd program reported the wrong status");

		if (nxu_exec("/disk/System/Library/CoreServices/no-such-program", 0) != -NXU_SYS_E_NOT_FOUND) return fail(24, "exec of a missing file did not fail cleanly");
	}

	/* 7. A caught signal runs its handler and execution resumes where it was. */
	{
		if (nxu_signal(NXU_SIGUSR1, handler_usr1) != 0) return fail(25, "sigaction failed");

		uint64_t before = 0x1234567890ABCDEFULL;
		g_handler_hits = 0ULL;

		if (nxu_kill((uint64_t)nxu_getpid(), NXU_SIGUSR1) != 0) return fail(26, "kill(self) failed");

		if (g_handler_hits != 1ULL || g_last_signal != (int)NXU_SIGUSR1) return fail(27, "handler did not run once with the signal number");
		if (before != 0x1234567890ABCDEFULL) return fail(28, "registers not restored after the handler");
		if ((g_mask_inside_handler & (1U << NXU_SIGUSR1)) == 0U) return fail(29, "signal not blocked while its handler runs");
		if ((nxu_sigprocmask(NXU_SIG_BLOCK, 0U) & (1U << NXU_SIGUSR1)) != 0) return fail(30, "signal still blocked after the handler returned");
	}

	/* 8. A blocked signal waits until it is unblocked. */
	{
		g_handler_hits = 0ULL;

		if (nxu_sigprocmask(NXU_SIG_BLOCK, 1U << NXU_SIGUSR1) != 0) return fail(31, "sigprocmask returned a stale mask");
		if (nxu_kill((uint64_t)nxu_getpid(), NXU_SIGUSR1) != 0) return fail(32, "kill(self) failed");

		(void)nxu_yield();
		if (g_handler_hits != 0ULL) return fail(33, "blocked signal was delivered");

		(void)nxu_sigprocmask(NXU_SIG_UNBLOCK, 1U << NXU_SIGUSR1);
		if (g_handler_hits != 1ULL) return fail(34, "unblocked signal was not delivered");
	}

	/* 9. Ignored signals vanish; SIGKILL cannot be caught. */
	{
		if (nxu_signal(NXU_SIGUSR2, (void (*)(int))NXU_SIG_IGN) != 0) return fail(35, "could not ignore SIGUSR2");
		if (nxu_kill((uint64_t)nxu_getpid(), NXU_SIGUSR2) != 0) return fail(36, "kill(self, ignored) failed");
		(void)nxu_yield();

		if (nxu_signal(NXU_SIGKILL, handler_ignore_marker) >= 0) return fail(37, "SIGKILL handler was accepted");
	}

	/* 10. Default action kills the target, and waitpid reports the signal. */
	{
		int64_t pid = nxu_fork();
		if (pid < 0) return fail(38, "fork failed");
		if (pid == 0) for (;;) (void)nxu_yield();

		if (nxu_kill((uint64_t)pid, NXU_SIGTERM) != 0) return fail(39, "kill(child, SIGTERM) failed");
		if (wait_child(pid, &status) != pid) return fail(40, "child did not die");
		if (!NXU_EXIT_WAS_KILLED(status) || NXU_EXIT_TERMSIG(status) != NXU_SIGTERM) return fail(41, "status does not record SIGTERM");
	}

	/* 11. A child that ignores SIGTERM survives it; SIGKILL still ends it. */
	{
		int64_t shared_id = nxu_shm_create(4096ULL);
		volatile uint64_t *shared = (volatile uint64_t *)nxu_shm_map((uint64_t)shared_id);
		if (shared_id <= 0 || (int64_t)shared <= 0) return fail(42, "shared memory unavailable");

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(43, "fork failed");

		if (pid == 0) {
			(void)nxu_signal(NXU_SIGTERM, (void (*)(int))NXU_SIG_IGN);
			shared[SHARED_CHILD_ARMED] = 1ULL;
			for (;;) (void)nxu_yield();
		}

		if (!wait_flag(&shared[SHARED_CHILD_ARMED])) return fail(44, "child never armed");

		if (nxu_kill((uint64_t)pid, NXU_SIGTERM) != 0) return fail(45, "kill(SIGTERM) failed");
		(void)nxu_yield();
		if (nxu_waitpid((uint64_t)pid, &status) != -NXU_SYS_E_AGAIN) return fail(46, "ignored SIGTERM killed the child");

		if (nxu_kill((uint64_t)pid, NXU_SIGKILL) != 0) return fail(47, "kill(SIGKILL) failed");
		if (wait_child(pid, &status) != pid) return fail(48, "SIGKILL did not end the child");
		if (!NXU_EXIT_WAS_KILLED(status) || NXU_EXIT_TERMSIG(status) != NXU_SIGKILL) return fail(49, "status does not record SIGKILL");
	}

	/* 12. SIGCHLD reaches a parent that asked for it. */
	{
		g_child_signals = 0ULL;
		if (nxu_signal(NXU_SIGCHLD, handler_chld) != 0) return fail(50, "sigaction(SIGCHLD) failed");

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(51, "fork failed");
		if (pid == 0) (void)nxu_exit(0ULL);

		if (!wait_flag(&g_child_signals)) return fail(52, "SIGCHLD never arrived");
		if (wait_child(pid, &status) != pid || status != 0ULL) return fail(53, "child status wrong after SIGCHLD");

		(void)nxu_signal(NXU_SIGCHLD, (void (*)(int))NXU_SIG_DFL);
	}

	/* 13. A fault runs the program's SIGSEGV handler instead of killing it. */
	{
		int64_t pid = nxu_fork();
		if (pid < 0) return fail(54, "fork failed");

		if (pid == 0) {
			(void)nxu_signal(NXU_SIGSEGV, handler_segv_exit);
			*(volatile uint64_t *)0 = 1ULL;
			(void)nxu_exit(1ULL);
		}

		if (wait_child(pid, &status) != pid || status != 55ULL) return fail(55, "SIGSEGV handler did not run");
	}

	/* 14. Signalling is limited to yourself and your descendants. */
	{
		if (nxu_kill(0ULL, 0U) != -NXU_SYS_E_DENIED) return fail(56, "signalling the kernel process was allowed");

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(57, "fork failed");

		if (pid == 0) {
			int64_t grandchild = nxu_fork();
			if (grandchild == 0) {
				/* Our parent is an ancestor, not a descendant. */
				(void)nxu_exit(nxu_kill((uint64_t)nxu_getppid(), 0U) == -NXU_SYS_E_DENIED ? 0ULL : 1ULL);
			}

			uint64_t grandchild_status = 1ULL;
			if (grandchild < 0 || wait_child(grandchild, &grandchild_status) != grandchild) (void)nxu_exit(2ULL);
			(void)nxu_exit(grandchild_status);
		}

		if (wait_child(pid, &status) != pid || status != 0ULL) return fail(58, "a descendant was allowed to signal its ancestor");
	}

	/* 15. wait() sleeps until a child exits; ANY takes whichever comes first. */
	{
		if (nxu_wait(NXU_WAIT_ANY, &status) != -NXU_SYS_E_NOT_FOUND) return fail(59, "wait with no children did not report NOT_FOUND");

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(60, "fork failed");
		if (pid == 0) {
			for (uint32_t index = 0U; index < 20U; index++) (void)nxu_yield();
			(void)nxu_exit(9ULL);
		}

		if (nxu_wait((uint64_t)pid, &status) != pid || status != 9ULL) return fail(61, "wait did not return the child's status");

		int64_t first = nxu_fork();
		if (first < 0) return fail(62, "fork failed");
		if (first == 0) (void)nxu_exit(21ULL);

		int64_t second = nxu_fork();
		if (second < 0) return fail(63, "fork failed");
		if (second == 0) (void)nxu_exit(22ULL);

		uint64_t total = 0ULL;
		for (uint32_t index = 0U; index < 2U; index++) {
			int64_t reaped = nxu_wait(NXU_WAIT_ANY, &status);
			if (reaped != first && reaped != second) return fail(64, "wait(ANY) returned a stranger");
			total += status;
		}

		if (total != 43ULL) return fail(65, "wait(ANY) did not collect both children");
		if (nxu_wait(NXU_WAIT_ANY, &status) != -NXU_SYS_E_NOT_FOUND) return fail(66, "wait after reaping everything did not report NOT_FOUND");
		if (nxu_wait(999ULL, &status) != -NXU_SYS_E_NOT_FOUND) return fail(67, "wait for a stranger did not report NOT_FOUND");
	}

	/* 16. A signal wakes a sleeping wait(): it returns INTERRUPTED. */
	{
		g_handler_hits = 0ULL;

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(68, "fork failed");
		if (pid == 0) for (;;) (void)nxu_yield();

		if (nxu_thread_spawn(signal_self_thread, 0, PROCTEST_THREAD_STACK) <= 0) return fail(69, "thread_spawn failed");

		if (nxu_wait((uint64_t)pid, &status) != -NXU_SYS_E_INTERRUPTED) return fail(70, "wait was not interrupted by the signal");
		if (!wait_flag(&g_handler_hits)) return fail(71, "the signal's handler never ran");

		/* The child is still running: the interrupted wait must not have reaped it. */
		if (nxu_kill((uint64_t)pid, NXU_SIGKILL) != 0) return fail(72, "kill failed");
		if (nxu_wait((uint64_t)pid, &status) != pid) return fail(73, "wait after the interrupt failed");
		if (!NXU_EXIT_WAS_KILLED(status) || NXU_EXIT_TERMSIG(status) != NXU_SIGKILL) return fail(74, "status does not record SIGKILL");
	}

	/* 17. A process asleep in the kernel can be killed, and nothing is left
	 * pointing at it (the wait queue, the port). Later sleeps still work. */
	{
		int64_t shared_id = nxu_shm_create(4096ULL);
		volatile uint64_t *shared = (volatile uint64_t *)nxu_shm_map((uint64_t)shared_id);
		if (shared_id <= 0 || (int64_t)shared <= 0) return fail(75, "shared memory unavailable");

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(76, "fork failed");

		if (pid == 0) {
			int64_t port = nxu_ipc_port_allocate();
			if (port <= 0) (void)nxu_exit(1ULL);

			shared[SHARED_CHILD_ARMED] = 1ULL;
			char scratch[16];
			(void)nxu_ipc_receive_wait((uint32_t)port, scratch, sizeof(scratch), 0, 0);
			(void)nxu_exit(2ULL);
		}

		if (!wait_flag(&shared[SHARED_CHILD_ARMED])) return fail(77, "child never went to sleep");
		for (uint32_t index = 0U; index < 50U; index++) (void)nxu_yield();

		if (nxu_kill((uint64_t)pid, NXU_SIGKILL) != 0) return fail(78, "kill of a sleeping process failed");
		if (wait_child(pid, &status) != pid) return fail(79, "sleeping process did not die");
		if (!NXU_EXIT_WAS_KILLED(status) || NXU_EXIT_TERMSIG(status) != NXU_SIGKILL) return fail(80, "status does not record SIGKILL");
	}

	/* 18. Blocking receive: a message sent by a child wakes the parent. */
	{
		int64_t port = nxu_ipc_port_allocate();
		if (port <= 0) return fail(81, "port allocate failed");

		int64_t pid = nxu_fork();
		if (pid < 0) return fail(82, "fork failed");

		if (pid == 0) {
			for (uint32_t index = 0U; index < 10U; index++) (void)nxu_yield();
			(void)nxu_exit(nxu_ipc_send((uint32_t)port, "hello", 5ULL, 0U) == 0 ? 0ULL : 1ULL);
		}

		char message[64];
		if (nxu_ipc_receive_wait((uint32_t)port, message, sizeof(message), 0, 0) != 5) return fail(83, "blocking receive did not return the message size");
		if (message[0] != 'h' || message[4] != 'o') return fail(84, "blocking receive returned the wrong bytes");

		if (nxu_wait((uint64_t)pid, &status) != pid || status != 0ULL) return fail(85, "sender failed");
		(void)nxu_ipc_port_deallocate((uint32_t)port);
	}

	say("proctest: fork, exec, signals, copy-on-write, demand paging and blocking waits all behaved\n");
	return 0;
}
