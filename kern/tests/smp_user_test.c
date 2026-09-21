/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/smp_user_test.c
 *
 * See kern/tests/smp_user_test.h.
 */

#include <kern/tests/smp_user_test.h>

#include <kern/console/console.h>
#include <kern/cpuset.h>
#include <kern/loader/elf.h>
#include <kern/machine/smp.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/processor.h>
#include <kern/sched_prism/sched.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>

#define SMP_USER_TEST_TIMEOUT_US 240000000ULL

/* Pages the run may legitimately keep (page-table caches, bookkeeping), not a leak. */
#define SMP_USER_TEST_LEAK_TOLERANCE_PAGES 32ULL

bool smp_user_test_run(void)
{
	uint32_t cpus = smp_online_count();

	if (cpus < 2U) {
		kputln("smp_user_test: FAILED: fewer than two CPUs online");
		return false;
	}

	/* Threads of user processes may run on every CPU from here on. */
	nxu_cpuset_t everywhere;

	cpuset_fill(&everywhere, NXU_MAX_CPUS);
	processor_set_default_user_affinity(&everywhere);

	uint64_t dispatches_before[NXU_MAX_CPUS];

	for (uint32_t cpu = 0U; cpu < cpus; cpu++) dispatches_before[cpu] = processor_by_id(cpu)->user_dispatch_count;

	uint64_t free_before = pmm_get_free_page_count();

	sched_user_running_reset_peak();

	proc_t proc = 0;
	loader_status_t status = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/smptest", "smptest", &proc);

	if (status != LOADER_STATUS_OK) {
		kprintf("smp_user_test: FAILED: spawning smptest: %s\n", loader_status_name(status));
		return false;
	}

	proc_id_t pid = proc->p_ident.pid;
	uint64_t exit_status = 0ULL;
	uint64_t deadline = timer_get_microseconds() + SMP_USER_TEST_TIMEOUT_US;
	bool reaped = false;

	while (!reaped && timer_get_microseconds() < deadline) {
		reaped = proc_reap(proc_kernel(), pid, &exit_status);

		if (!reaped) (void)sched_yield();
	}

	if (!reaped) {
		kputln("smp_user_test: FAILED: smptest did not finish");
		return false;
	}

	if (exit_status != 0ULL) {
		kprintf("smp_user_test: FAILED: smptest exited with status %llu (its own lines above say which case)\n", (unsigned long long)exit_status);
		return false;
	}

	uint32_t peak = sched_user_running_peak();
	uint32_t cpus_used = 0U;

	for (uint32_t cpu = 0U; cpu < cpus; cpu++) {
		uint64_t dispatched = processor_by_id(cpu)->user_dispatch_count - dispatches_before[cpu];

		if (dispatched != 0ULL) cpus_used++;

		kprintf("smp_user_test: cpu%u ran user threads %llu times\n", cpu, (unsigned long long)dispatched);
	}

	kprintf("smp_user_test: user threads ran on %u of %u CPUs; at most %u CPUs ran them at the same time\n", cpus_used, cpus, peak);

	uint32_t wanted = cpus < 3U ? cpus : 3U;

	if (cpus_used < wanted) {
		kputln("smp_user_test: FAILED: user threads did not spread over the CPUs");
		return false;
	}

	if (peak < wanted) {
		kputln("smp_user_test: FAILED: too few CPUs ever ran user threads at the same time");
		return false;
	}

	if (!sched_validate_all()) {
		kputln("smp_user_test: FAILED: the run queues are inconsistent afterwards");
		return false;
	}

	/* Every process is gone: every page they used, and their page tables, must be back. */
	uint64_t free_after = pmm_get_free_page_count();

	kprintf("smp_user_test: free pages before %llu, after %llu\n", (unsigned long long)free_before, (unsigned long long)free_after);

	if (free_after + SMP_USER_TEST_LEAK_TOLERANCE_PAGES < free_before) {
		kputln("smp_user_test: FAILED: pages were leaked");
		return false;
	}

	return true;
}
