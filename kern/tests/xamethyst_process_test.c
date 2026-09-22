/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/xamethyst_process_test.c
 *
 * Spawns XAmethyst (frameworks/BootDaemons.framework/xamethyst.c) for
 * real, then a throwaway raw-protocol client (x11test_handshake.c) that
 * connects via kern/ipc/socket.h's named registry, performs the real X11
 * connection-setup handshake, and sends one request to confirm the
 * request-read loop stays in sync afterward. XAmethyst itself is left
 * running (a long-lived service, like bootd); only the test client is
 * expected to exit.
 */

#include <kern/tests/xamethyst_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>

#define XAMETHYST_PROCESS_TEST_WAIT_ITERATIONS 2000000U
#define XAMETHYST_PROCESS_TEST_STARTUP_WAIT_ITERATIONS 1000U

static bool
xamethyst_process_test_wait(proc_t parent, proc_id_t pid, uint64_t *status)
{
	for (uint32_t spin = 0U; spin < XAMETHYST_PROCESS_TEST_WAIT_ITERATIONS; spin++) {
		if (proc_reap(parent, pid, status)) return true;
		if (!sched_yield()) return false;
	}
	return false;
}

bool
xamethyst_process_test(void)
{
	proc_t proc_xamethyst = 0;
	proc_t proc_client = 0;

	loader_status_t status_xamethyst = loader_spawn_caps(proc_kernel(), "/disk/System/Library/CoreServices/xamethyst", "xamethyst", NXU_CAP_DISPLAY, &proc_xamethyst);
	if (status_xamethyst != LOADER_STATUS_OK) {
		kputs("amethyst_test_process: spawning xamethyst failed: ");
		kputln(loader_status_name(status_xamethyst));
		return false;
	}

	/*
	 * XAmethyst registers its listening socket only after claiming the
	 * display and allocating the shadow framebuffer; x11test_handshake
	 * already retries nxu_socket_connect on its own, but giving XAmethyst
	 * real scheduler turns first keeps this test's own failure modes
	 * easier to read apart, matching windowserver_process_test.c's same
	 * precaution.
	 */
	for (uint32_t spin = 0U; spin < XAMETHYST_PROCESS_TEST_STARTUP_WAIT_ITERATIONS; spin++) {
		if (!sched_yield()) {
			kputln("amethyst_test_process: scheduler dispatch failed while waiting for xamethyst");
			return false;
		}
	}

	loader_status_t status_client = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/x11test_handshake", "x11test_handshake", &proc_client);
	if (status_client != LOADER_STATUS_OK) {
		kputs("amethyst_test_process: spawning x11test_handshake failed: ");
		kputln(loader_status_name(status_client));
		return false;
	}

	uint64_t exit_client = 0ULL;
	if (!xamethyst_process_test_wait(proc_kernel(), proc_client->p_ident.pid, &exit_client)) {
		kputln("amethyst_test_process: x11test_handshake did not exit in time");
		return false;
	}

	if (exit_client != 0ULL) {
		kprintf("amethyst_test_process: x11test_handshake exited with nonzero status %llu\n", (unsigned long long)exit_client);
		return false;
	}

	(void)proc_xamethyst;

	kputln("amethyst_test_process: XAmethyst served a real X11 connection-setup handshake and stayed in sync afterward");

	/*
	 * x11test_input (frameworks/BootDaemons.framework/x11test_input.c) is
	 * left running, like XAmethyst itself: it creates a full-screen
	 * window and waits for real input events, which this automated test
	 * has no way to inject. It's verified manually -- QEMU screendumps
	 * before/after sending a synthetic click/keypress through the QEMU
	 * monitor -- rather than by exit code.
	 */
	proc_t proc_input_test = 0;
	loader_status_t status_input_test = loader_spawn(proc_kernel(), "/disk/System/Library/CoreServices/x11test_input", "x11test_input", &proc_input_test);
	if (status_input_test != LOADER_STATUS_OK) {
		kputs("amethyst_test_process: spawning x11test_input failed (non-fatal, input delivery must be checked manually): ");
		kputln(loader_status_name(status_input_test));
	} else {
		kputs("amethyst_test_process: x11test_input spawned as PID ");
		kputu64(proc_input_test->p_ident.pid);
		kputc('\n');

		for (uint32_t spin = 0U; spin < XAMETHYST_PROCESS_TEST_STARTUP_WAIT_ITERATIONS; spin++) {
			if (!sched_yield()) break;
		}

		kputln("amethyst_test_process: gave x11test_input scheduler turns");
	}

	return true;
}
