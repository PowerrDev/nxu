/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/process_control_test.c
 *
 * Spawns proctest (frameworks/BootDaemons.framework/proctest.c), which
 * drives fork, exec, signals, copy-on-write and demand paging from real
 * user code and exits 0 only if every check held. The kernel then checks
 * the one thing user code cannot: that all of it -- a dozen forked address
 * spaces, an exec'd image, a 16 MiB lazy region -- was given back, by
 * comparing the physical pages in use before and after.
 */

#include <kern/tests/process_control_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>

#define PROCESS_CONTROL_TEST_WAIT_ITERATIONS 4000000U

/*
 * The kernel heap and the vm_kern page tables legitimately retain a few
 * pages once they have grown; a leaked address space would be tens of pages
 * per fork, and proctest forks over a dozen.
 */
#define PROCESS_CONTROL_TEST_LEAK_TOLERANCE_PAGES 24ULL

static bool
process_control_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < PROCESS_CONTROL_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

/*
 * The positive half of the capability tests. The kernel, not bootd, starts
 * privtest, so it holds nothing until it is granted what it exercises: writing
 * the filesystem and claiming the display, but not resetting the machine.
 */
static bool
process_control_test_privileges(void)
{
	proc_t proc = 0;
	loader_status_t loaded = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/privtest", "privtest", &proc);
	if (loaded != LOADER_STATUS_OK) {
		kputs("process_control_test: spawning privtest failed: ");
		kputln(loader_status_name(loaded));
		return false;
	}

	proc_set_caps(proc, NXU_CAP_FS_WRITE | NXU_CAP_DISPLAY);

	uint64_t exit_status = 0ULL;
	if (!process_control_test_wait(proc_kernel(), proc->p_ident.pid, &exit_status)) {
		kputln("process_control_test: privtest did not exit in time");
		return false;
	}

	if (exit_status != 0ULL) {
		kprintf("process_control_test: privtest failed with status 0x%llx (check number, or a kill)\n", (unsigned long long)exit_status);
		return false;
	}

	return true;
}

static bool
process_control_test_run(bool exclusive)
{
	uint64_t used_before = pmm_get_used_page_count();

	proc_t proc = 0;
	loader_status_t loaded = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/proctest", "proctest", &proc);
	if (loaded != LOADER_STATUS_OK) {
		kputs("process_control_test: spawning proctest failed: ");
		kputln(loader_status_name(loaded));
		return false;
	}

	/*
	 * Nothing else was started, so proctest is PID 1 and would hold every
	 * capability. It checks that an ordinary process is refused, so take them
	 * away before it runs.
	 */
	proc_set_caps(proc, 0U);

	uint64_t exit_status = 0ULL;
	if (!process_control_test_wait(proc_kernel(), proc->p_ident.pid, &exit_status)) {
		kputln("process_control_test: proctest did not exit in time");
		return false;
	}

	if (exit_status != 0ULL) {
		kprintf("process_control_test: proctest failed with status 0x%llx (check number, or a kill)\n", (unsigned long long)exit_status);
		return false;
	}

	if (!process_control_test_privileges()) return false;

	uint64_t used_after = pmm_get_used_page_count();
	uint64_t leaked = used_after > used_before ? used_after - used_before : 0ULL;

	kprintf("process_control_test: pages in use before %llu, after %llu\n", (unsigned long long)used_before, (unsigned long long)used_after);

	/*
	 * With other work running (the unified boot) the page count moves for
	 * reasons that are not proctest's: the audio thread reads a WAV, a
	 * daemon starts. The count is printed, but it proves nothing there.
	 */
	if (!exclusive) {
		kputln("process_control_test: page reclaim not checked: other work is running in this boot");
		kputln("process_control_test: fork, exec, signals, copy-on-write, demand paging and capabilities all worked");
		return true;
	}

	if (leaked > PROCESS_CONTROL_TEST_LEAK_TOLERANCE_PAGES) {
		kprintf("process_control_test: %llu pages were not given back\n", (unsigned long long)leaked);
		return false;
	}

	kputln("process_control_test: fork, exec, signals, copy-on-write, demand paging and capabilities all worked and every page was reclaimed");
	return true;
}

bool
process_control_test(void)
{
	return process_control_test_run(true);
}

bool
process_control_test_shared(void)
{
	return process_control_test_run(false);
}
