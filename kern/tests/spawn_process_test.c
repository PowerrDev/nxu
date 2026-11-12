/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/spawn_process_test.c
 *
 * Starts the user program spawntest
 * (frameworks/BootDaemons.framework/spawntest.c) and waits for its verdict.
 * spawntest spawns itself with arguments, chosen descriptors, process groups,
 * a new session and a signal mask, and checks from both the parent's and the
 * child's side that each was in place before the child ran.
 *
 * spawntest runs as an ordinary user holding no capabilities: nothing it
 * exercises needs any.
 */

#include <kern/tests/spawn_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/security/ucred.h>

#include <stdbool.h>
#include <stdint.h>

#define SPAWN_PROCESS_TEST_PATH "/Volumes/mainOS/System/Library/CoreServices/spawntest"
#define SPAWN_PROCESS_TEST_TIMEOUT_US 60000000ULL

bool
spawn_process_test(void)
{
	proc_t proc = 0;
	loader_status_t loaded =
		loader_spawn_as(proc_kernel(), SPAWN_PROCESS_TEST_PATH, "spawntest", 0U, NXU_UID_NOBODY, NXU_GID_NOBODY, &proc);

	if (loaded != LOADER_STATUS_OK) {
		kprintf("spawning spawntest failed: %s\n", loader_status_name(loaded));
		return false;
	}

	uint64_t status = 0ULL;
	uint64_t deadline = timer_get_microseconds() + SPAWN_PROCESS_TEST_TIMEOUT_US;

	while (!proc_reap(proc_kernel(), proc->p_ident.pid, &status)) {
		if (timer_get_microseconds() > deadline || !sched_yield()) {
			kprintf("spawntest did not exit in time\n");
			return false;
		}
	}

	if (status != 0ULL) {
		kprintf("spawntest failed with status 0x%llx (check number, or a kill)\n",
				(unsigned long long)status);
		return false;
	}

	kprintf("a spawned process starts with its arguments, descriptors, group and mask in place\n");
	return true;
}
