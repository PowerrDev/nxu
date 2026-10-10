#include <nxu/syscall.h>
#include <nxu/string.h>
#include <nxu/thread.h>

/* Run manually, or as a bootd job with crash.test=1. A recovered signal and a
 * regular exit are covered separately by process-control's existing tests. */
__attribute__((noinline)) static void crash_leaf(void)
{
	*(volatile uint64_t *)(uintptr_t)0x12345000ULL = 42ULL;
	__asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) static void crash_middle(void)
{
	crash_leaf();
	__asm__ volatile("" ::: "memory");
}

static volatile uint32_t g_workers;

static void crash_worker(void *arg)
{
	__atomic_fetch_add(&g_workers, 1U, __ATOMIC_RELEASE);
	if ((uintptr_t)arg == 0U)
		for (;;)
			__asm__ volatile("nop" ::: "memory");
	for (;;)
		(void)nxu_sleep_us(10000ULL);
}

static void caught_abort(int signal)
{
	(void)signal;
	(void)nxu_exit(0);
}

static int child_case(uint32_t kind)
{
	int64_t child = nxu_fork();

	if (child < 0)
		return 1;

	if (child == 0) {
		if (kind == 0)
			return (int)nxu_exit(0);

		if (kind == 1 && nxu_signal(NXU_SIGABRT, caught_abort) < 0)
			return 2;

		if (kind == 3) {
#if defined(__aarch64__)
			__asm__ volatile(".inst 0");
#endif
		} else
			(void)nxu_kill((uint64_t)nxu_getpid(), NXU_SIGABRT);
		return 3;
	}

	uint64_t status;

	if (nxu_wait((uint64_t)child, &status) < 0)
		return 4;

	if (kind < 2 && status != 0)
		return 5;

	if (kind >= 2 &&
		(!NXU_EXIT_WAS_KILLED(status) || NXU_EXIT_TERMSIG(status) != (kind == 3 ? NXU_SIGILL : NXU_SIGABRT)))
		return 6;

	return 0;
}

int main(void)
{
	char args[512];

	if (nxu_get_boot_args(args, sizeof(args)) >= 0 && nxu_arg_present(args, "crash.test=abort")) {
		(void)nxu_kill((uint64_t)nxu_getpid(), NXU_SIGABRT);
		return 1;
	}

	if (nxu_get_boot_args(args, sizeof(args)) >= 0 && nxu_arg_present(args, "crash.test=suite")) {
		if (nxu_crash(NXU_CRASH_NEXT, 0) != -NXU_SYS_E_DENIED || nxu_crash(NXU_CRASH_UI_NEXT, 0) != -NXU_SYS_E_DENIED ||
			nxu_crash(NXU_CRASH_NOTIFY, 0) != -NXU_SYS_E_DENIED)
			return 10;

		for (uint32_t kind = 0; kind < 4; kind++)
			if (child_case(kind))
				return 11;

		if (nxu_thread_spawn(crash_worker, 0, 16384) < 0 || nxu_thread_spawn(crash_worker, (void *)1, 16384) < 0)
			return 12;

		while (__atomic_load_n(&g_workers, __ATOMIC_ACQUIRE) != 2U)
			(void)nxu_sleep_us(1000);

		const char *ok = "crashtest: normal exit, caught signal, access control, SIGABRT and SIGILL passed\n";
		(void)nxu_write(1, ok, nxu_strlen(ok));
	}

	crash_middle();
	return 1;
}
