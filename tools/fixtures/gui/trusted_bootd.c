/* Private PID 1 supervisor for restart during a trusted modal. */
#include <frameworks/CoreFoundation.framework/sbin/bootd/manager.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static bootd_manager_t manager;
static uint32_t restart_requested;

static void
request_restart(void *unused)
{
	(void)unused;
	nxu_ui_session_t session;

	if (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)(uintptr_t)&session) < 0) (void)nxu_thread_exit(0);

	(void)nxu_sleep_us(3000000);
	__atomic_store_n(&restart_requested, 1, __ATOMIC_RELEASE);
	(void)nxu_thread_exit(0);
}

int
main(void)
{
	if (nxu_getpid() != 1 || !bootd_manager_init(&manager)) return 1;

	bootd_manager_start(&manager);
	int64_t stack = nxu_mmap(16384, 0);

	if (stack <= 0 || nxu_thread_create(request_restart, (void *)(uintptr_t)(stack + 16384), 0) < 0) return 2;

	for (;;) {
		bootd_manager_poll(&manager);

		if (__atomic_exchange_n(&restart_requested, 0, __ATOMIC_ACQ_REL)) {
			for (uint32_t i = 0; i < manager.job_count; i++) {
				bootd_job_t *job = &manager.jobs[i];

				if (nxu_streq(job->config.label, "com.nxu.windowserver") && job->pid) {
					if (nxu_kill(job->pid, 9) < 0) return 3;

					static const char marker[] = "trusted-manager: compositor killed during modal\n";
					(void)nxu_write(1, marker, sizeof(marker) - 1);
				}
			}
		}

		(void)nxu_sleep_us(1000);
	}
}
