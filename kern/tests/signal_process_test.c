/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/signal_process_test.c
 *
 * Starts the user program sigtest (frameworks/BootDaemons.framework/sigtest.c)
 * and waits for its verdict. sigtest exercises signal delivery without fork:
 * handler frames and sigreturn in one process, then stop, continue and kill
 * between itself and a child it spawns. It is the signal test of a port that
 * has no fork; where fork exists, proctest covers the same ground and more.
 *
 * sigtest runs as an ordinary user, not as root, like the other tests whose
 * subject is what an unprivileged process may do.
 */

#include <kern/tests/signal_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/security/ucred.h>

#include <stdbool.h>
#include <stdint.h>

#define SIGNAL_PROCESS_TEST_PATH "/Volumes/mainOS/System/Library/CoreServices/sigtest"
#define SIGNAL_PROCESS_TEST_TIMEOUT_US 60000000ULL

bool
signal_process_test(void)
{
	proc_t proc = 0;
	loader_status_t loaded =
		loader_spawn_as(proc_kernel(), SIGNAL_PROCESS_TEST_PATH, "sigtest", 0U, NXU_UID_NOBODY, NXU_GID_NOBODY, &proc);

	if (loaded != LOADER_STATUS_OK) {
		kprintf("signal_process_test: spawning sigtest failed: %s\n", loader_status_name(loaded));
		return false;
	}

	uint64_t status = 0ULL;
	uint64_t deadline = timer_get_microseconds() + SIGNAL_PROCESS_TEST_TIMEOUT_US;

	while (!proc_reap(proc_kernel(), proc->p_ident.pid, &status)) {
		if (timer_get_microseconds() > deadline || !sched_yield()) {
			kputln("signal_process_test: sigtest did not exit in time");
			return false;
		}
	}

	if (status != 0ULL) {
		kprintf("signal_process_test: sigtest failed with status 0x%llx (check number, or a kill)\n",
				(unsigned long long)status);
		return false;
	}

	kputln("signal_process_test: signal handlers, stop, continue and kill work between spawned processes");
	return true;
}
