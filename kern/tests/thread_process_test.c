/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/thread_process_test.c
 *
 * Spawns threadtest (frameworks/BootDaemons.framework/threadtest.c) for
 * real and waits for it to exit. threadtest spawns several of its own
 * threads via nxu_thread_spawn (NXU_SYS_MMAP for a private stack +
 * NXU_SYS_THREAD_CREATE) and checks that every one of them actually ran,
 * with the caller-supplied argument correctly delivered -- the end-to-end
 * proof that a real user process, not just the kernel's own bootstrap
 * thread, can create and run additional threads of its own.
 */

#include <kern/tests/thread_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

#define THREAD_PROCESS_TEST_WAIT_ITERATIONS 2000000U

static bool
thread_process_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < THREAD_PROCESS_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

bool
thread_process_test(void)
{
	proc_t proc = 0;

	loader_status_t status = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/threadtest", "threadtest", &proc);
	if (status != LOADER_STATUS_OK) {
		kputs("thread_process_test: spawning threadtest failed: ");
		kputln(loader_status_name(status));
		return false;
	}

	uint64_t exit_status = 0ULL;
	if (!thread_process_test_wait(proc_kernel(), proc->p_ident.pid, &exit_status)) {
		kputln("thread_process_test: threadtest did not exit in time");
		return false;
	}

	if (exit_status != 0ULL) {
		kputs("thread_process_test: threadtest exited with nonzero status: ");
		kputu64(exit_status);
		kputc('\n');
		return false;
	}

	kputln("thread_process_test: real process spawned its own threads and all ran correctly");
	return true;
}
