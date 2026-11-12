/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/ipc_process_test.c
 *
 * Spawns bootd for real (PID 1, hosting the bootstrap registry -- see
 * frameworks/CoreFoundation.framework/sbin/bootd/registry.c), then two
 * more real userland processes (ipctest_a, ipctest_b -- see
 * frameworks/BootDaemons.framework/ipctest_a.c / ipctest_b.c) that find
 * each other purely by registering/looking up a name through that
 * registry, exchange a message with a port transfer, and reply. This is
 * the end-to-end proof that NXPC's bootstrap discovery works across real
 * process boundaries: kern/loader/elf.c hands every spawned process (these
 * two included, though the kernel test harness -- not bootd -- is their
 * direct parent) a send right to whatever bootd has registered as the
 * system bootstrap registry by the time they're spawned, and the registry
 * itself is plain userspace code (frameworks/.../bootstrap_client.c)
 * using the same syscalls as everything else.
 */

#include <kern/tests/ipc_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/ipc/ipc_init.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

#define IPC_PROCESS_TEST_WAIT_ITERATIONS 2000000U
#define IPC_PROCESS_TEST_REGISTRY_WAIT_ITERATIONS 2000000U

static bool
ipc_process_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < IPC_PROCESS_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;

		if (!sched_yield()) return false;
	}
	return false;
}

static bool
ipc_process_test_wait_registry(void)
{
	for (uint32_t spin = 0U; spin < IPC_PROCESS_TEST_REGISTRY_WAIT_ITERATIONS; spin++) {
		if (ipc_bootstrap_registry_port() != IPC_PORT_NULL) return true;

		if (!sched_yield()) return false;
	}
	return false;
}

bool
ipc_process_test(void)
{
	proc_t proc_bootd = 0;
	proc_t proc_a = 0;
	proc_t proc_b = 0;
	bool passed = false;

	/* A boot that already runs bootd (the unified boot) has the registry: a second bootd would replace it. */
	if (ipc_bootstrap_registry_port() == IPC_PORT_NULL) {
		loader_status_t status_bootd =
			loader_spawn(proc_kernel(), "/Volumes/mainOS/System/Library/CoreServices/bootd", "bootd", &proc_bootd);
		if (status_bootd != LOADER_STATUS_OK) {
			kputs("ipc_process_test: spawning bootd failed: ");
			kputln(loader_status_name(status_bootd));
			return false;
		}

	}

	if (!ipc_process_test_wait_registry()) {
		kprintf("bootstrap registry did not come up in time\n");
		goto cleanup;
	}

	kprintf("bootstrap registry ready, spawning ipctest_a and ipctest_b\n");

	loader_status_t status_a =
		loader_spawn(proc_kernel(), "/Volumes/mainOS/System/Library/CoreServices/ipctest_a", "ipctest_a", &proc_a);
	if (status_a != LOADER_STATUS_OK) {
		kputs("ipc_process_test: spawning ipctest_a failed: ");
		kputln(loader_status_name(status_a));
		goto cleanup;
	}

	loader_status_t status_b =
		loader_spawn(proc_kernel(), "/Volumes/mainOS/System/Library/CoreServices/ipctest_b", "ipctest_b", &proc_b);
	if (status_b != LOADER_STATUS_OK) {
		kputs("ipc_process_test: spawning ipctest_b failed: ");
		kputln(loader_status_name(status_b));
		goto cleanup;
	}

	uint64_t exit_a = 0ULL;
	uint64_t exit_b = 0ULL;
	if (!ipc_process_test_wait(proc_kernel(), proc_a->p_ident.pid, &exit_a)) {
		kprintf("ipctest_a did not exit in time\n");
		goto cleanup;
	}

	if (!ipc_process_test_wait(proc_kernel(), proc_b->p_ident.pid, &exit_b)) {
		kprintf("ipctest_b did not exit in time\n");
		goto cleanup;
	}

	if (exit_a != 0ULL || exit_b != 0ULL) {
		kprintf("nonzero exit status (a=%llu b=%llu)\n", (unsigned long long)exit_a,
				(unsigned long long)exit_b);
		goto cleanup;
	}

	passed = true;

cleanup:
	/*
	 * bootd itself is left running (it never exits on its own, matching
	 * production behavior) -- this test build never reaches the normal
	 * scheduler-dispatch loop, so it simply stops being scheduled once
	 * this function returns and the caller halts.
	 */
	if (!passed) return false;

	kprintf("two-process bootstrap register/lookup round trip passed (spawn, discover by name, port \n"
		   "transfer, reply, reap)");
	return true;
}
