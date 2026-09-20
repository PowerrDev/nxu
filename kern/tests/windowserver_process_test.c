/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/windowserver_process_test.c
 *
 * Spawns bootd (for the bootstrap registry), the standalone WindowServer
 * process (frameworks/BootDaemons.framework/windowserver_service.c), and a
 * throwaway client (wstest_client.c) that creates a window, renders a small
 * solid-color rectangle into it, and presents -- purely over NXPC, proving
 * WindowServer works as a real, separate OS process. windowserver_service
 * itself is left running afterward (it is a long-lived service, like
 * bootd); only wstest_client is expected to exit. The actual visual proof
 * (the rectangle landing on screen) is checked from outside the kernel via
 * a QEMU screendump once this returns and the caller halts.
 */

#include <kern/tests/windowserver_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/ipc/ipc_init.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

#define WINDOWSERVER_PROCESS_TEST_WAIT_ITERATIONS 2000000U
#define WINDOWSERVER_PROCESS_TEST_REGISTRY_WAIT_ITERATIONS 2000000U
#define WINDOWSERVER_PROCESS_TEST_SERVICE_WAIT_ITERATIONS 1000U

static bool
windowserver_process_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < WINDOWSERVER_PROCESS_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

static bool
windowserver_process_test_wait_registry(void)
{
	for (uint32_t spin = 0U; spin < WINDOWSERVER_PROCESS_TEST_REGISTRY_WAIT_ITERATIONS; spin++) {
		if (ipc_bootstrap_registry_port() != IPC_PORT_NULL) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

bool
windowserver_process_test(void)
{
	proc_t proc_bootd = 0;
	proc_t proc_windowserver = 0;
	proc_t proc_client = 0;

	loader_status_t status_bootd = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/bootd", "bootd", &proc_bootd);
	if (status_bootd != LOADER_STATUS_OK) {
		kputs("windowserver_process_test: spawning bootd failed: ");
		kputln(loader_status_name(status_bootd));
		return false;
	}

	if (!windowserver_process_test_wait_registry()) {
		kputln("windowserver_process_test: bootstrap registry did not come up in time");
		return false;
	}

	loader_status_t status_windowserver = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/windowserver_service", "windowserver_service", &proc_windowserver);
	if (status_windowserver != LOADER_STATUS_OK) {
		kputs("windowserver_process_test: spawning windowserver_service failed: ");
		kputln(loader_status_name(status_windowserver));
		return false;
	}

	/* Started by the kernel, not by bootd's plist, so it has no capabilities
	 * until we grant the one it needs: it claims the display. */
	proc_set_caps(proc_windowserver, NXU_CAP_DISPLAY);

	/*
	 * windowserver_service registers itself with the bootstrap registry
	 * only after claiming the display and initializing the compositor;
	 * give it real scheduler turns to get there before wstest_client
	 * starts looking it up (bootstrap_client_lookup already retries on
	 * its own, but starting the spawn only once the process is likely
	 * alive keeps this test's own failure modes easier to read apart).
	 */
	for (uint32_t spin = 0U; spin < WINDOWSERVER_PROCESS_TEST_SERVICE_WAIT_ITERATIONS; spin++) {
		if (!sched_yield()) {
			kputln("windowserver_process_test: scheduler dispatch failed while waiting for windowserver_service");
			return false;
		}
	}

	loader_status_t status_client = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/wstest_client", "wstest_client", &proc_client);
	if (status_client != LOADER_STATUS_OK) {
		kputs("windowserver_process_test: spawning wstest_client failed: ");
		kputln(loader_status_name(status_client));
		return false;
	}

	uint64_t exit_client = 0ULL;
	if (!windowserver_process_test_wait(proc_kernel(), proc_client->p_ident.pid, &exit_client)) {
		kputln("windowserver_process_test: wstest_client did not exit in time");
		return false;
	}

	if (exit_client != 0ULL) {
		kprintf("windowserver_process_test: wstest_client exited with nonzero status %llu\n", (unsigned long long)exit_client);
		return false;
	}

	(void)proc_windowserver;

	/*
	 * wstest_client's WSMSG_PRESENT is fire-and-forget (see
	 * frameworks/BootDaemons.framework/wstest_client.c) -- it exits right
	 * after the send succeeds, without waiting for WindowServer to have
	 * actually dequeued and processed it yet. Give WindowServer's process
	 * a bounded number of further scheduler turns so that last message
	 * gets handled (and the frame actually reaches the display) before
	 * this test returns and the caller stops yielding entirely.
	 */
	for (uint32_t spin = 0U; spin < WINDOWSERVER_PROCESS_TEST_SERVICE_WAIT_ITERATIONS; spin++) {
		if (!sched_yield()) break;
	}

	kputln("windowserver_process_test: WindowServer served a real client over NXPC (spawn, discover by name, create window, render, present)");
	return true;
}
