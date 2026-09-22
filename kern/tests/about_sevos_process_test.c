/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/about_sevos_process_test.c
 *
 * Spawns bootd (bootstrap registry), the standalone WindowServer process,
 * and About sevOS as its own real process (see
 * frameworks/BootDaemons.framework/about_sevos_service.c) that discovers
 * WindowServer by name, creates a window sized to match the real About
 * sevOS app, draws its content into a shared-memory region, and asks
 * WindowServer to composite and present it -- purely over NXPC/nxu_shm,
 * proving two independent GUI processes coexisting over the same
 * WindowServer. About sevOS is expected to exit after one render; the
 * visual result is checked from outside the kernel via a QEMU screendump
 * once this returns and the caller halts.
 */

#include <kern/tests/about_sevos_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/ipc/ipc_init.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

#define ABOUT_SEVOS_PROCESS_TEST_WAIT_ITERATIONS 2000000U
#define ABOUT_SEVOS_PROCESS_TEST_REGISTRY_WAIT_ITERATIONS 2000000U
#define ABOUT_SEVOS_PROCESS_TEST_SERVICE_WAIT_ITERATIONS 1000U

static bool
about_sevos_process_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < ABOUT_SEVOS_PROCESS_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

static bool
about_sevos_process_test_wait_registry(void)
{
	for (uint32_t spin = 0U; spin < ABOUT_SEVOS_PROCESS_TEST_REGISTRY_WAIT_ITERATIONS; spin++) {
		if (ipc_bootstrap_registry_port() != IPC_PORT_NULL) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

bool
about_sevos_process_test(void)
{
	proc_t proc_bootd = 0;
	proc_t proc_windowserver = 0;
	proc_t proc_about = 0;

	loader_status_t status_bootd = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/bootd", "bootd", &proc_bootd);
	if (status_bootd != LOADER_STATUS_OK) {
		kputs("about_sevos_process_test: spawning bootd failed: ");
		kputln(loader_status_name(status_bootd));
		return false;
	}

	if (!about_sevos_process_test_wait_registry()) {
		kputln("about_sevos_process_test: bootstrap registry did not come up in time");
		return false;
	}

	loader_status_t status_windowserver = loader_spawn_caps(proc_kernel(), "/disk/System/Library/CoreServices/windowserver_service", "windowserver_service", NXU_CAP_DISPLAY, &proc_windowserver);
	if (status_windowserver != LOADER_STATUS_OK) {
		kputs("about_sevos_process_test: spawning windowserver_service failed: ");
		kputln(loader_status_name(status_windowserver));
		return false;
	}

	for (uint32_t spin = 0U; spin < ABOUT_SEVOS_PROCESS_TEST_SERVICE_WAIT_ITERATIONS; spin++) {
		if (!sched_yield()) {
			kputln("about_sevos_process_test: scheduler dispatch failed while waiting for windowserver_service");
			return false;
		}
	}

	loader_status_t status_about = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/about_sevos_service", "about_sevos_service", &proc_about);
	if (status_about != LOADER_STATUS_OK) {
		kputs("about_sevos_process_test: spawning about_sevos_service failed: ");
		kputln(loader_status_name(status_about));
		return false;
	}

	uint64_t exit_about = 0ULL;
	if (!about_sevos_process_test_wait(proc_kernel(), proc_about->p_ident.pid, &exit_about)) {
		kputln("about_sevos_process_test: about_sevos_service did not exit in time");
		return false;
	}

	if (exit_about != 0ULL) {
		kprintf("about_sevos_process_test: about_sevos_service exited with nonzero status %llu\n", (unsigned long long)exit_about);
		return false;
	}

	(void)proc_windowserver;

	/*
	 * about_sevos_service's WSMSG_PRESENT is fire-and-forget (see
	 * wstest_client.c's identical pattern and windowserver_process_test.c's
	 * comment on the same wait) -- give WindowServer's process further
	 * scheduler turns so it actually dequeues and processes it before this
	 * test returns and the caller stops yielding entirely.
	 */
	for (uint32_t spin = 0U; spin < ABOUT_SEVOS_PROCESS_TEST_SERVICE_WAIT_ITERATIONS; spin++) {
		if (!sched_yield()) break;
	}

	kputln("about_sevos_process_test: About sevOS served as a real process over NXPC (spawn, discover by name, shared-memory render, present)");
	return true;
}
