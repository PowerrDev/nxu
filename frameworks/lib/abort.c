#include <nxu/runtime.h>
#include <nxu/syscall.h>

_Noreturn void nxu_runtime_abort(void)
{
	(void)nxu_signal(NXU_SIGABRT, (void (*)(int))(uintptr_t)NXU_SIG_DFL);
	(void)nxu_sigprocmask(NXU_SIG_UNBLOCK, 1U << NXU_SIGABRT);
	(void)nxu_kill((uint64_t)nxu_getpid(), NXU_SIGABRT);
	for (;;) {
		(void)nxu_exit(134);
		(void)nxu_sleep_us(100000);
	}
}
