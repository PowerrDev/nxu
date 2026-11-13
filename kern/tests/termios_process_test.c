/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/termios_process_test.c
 *
 * Starts the user program ttytest (frameworks/BootDaemons.framework/
 * ttytest.c) on a terminal of its own and waits for its verdict. The
 * terminal is the loopback test driver (kern/tests/tty_loopback.h) at
 * /dev/ttytest, open to everyone: ttytest runs as an ordinary user holding no
 * capabilities, because nothing a program does to its own terminal needs
 * any. It also runs stty (frameworks/BootDaemons.framework/stty.c) and reads
 * its output.
 *
 * When ttytest is done the terminal is taken down the way a driver that goes
 * away does (unregister, detach, release), and it must then be freed: every
 * file ttytest and stty opened on it has been closed by their exits.
 */

#include <kern/tests/termios_process_test.h>

#include <kern/console/console.h>
#include <kern/loader/elf.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/security/ucred.h>
#include <kern/tests/tty_loopback.h>
#include <kern/tty/tty.h>

#include <stdbool.h>
#include <stdint.h>

#define TERMIOS_TEST_PATH "/Volumes/mainOS/System/Library/CoreServices/ttytest"
#define TERMIOS_TEST_DEVICE "ttytest"
#define TERMIOS_TEST_TIMEOUT_US 60000000ULL

static tty_loopback_t g_terminal;

bool
termios_process_test(void)
{
	uint32_t terminals = tty_count();

	if (!tty_loopback_create(&g_terminal)) {
		kprintf("cannot create the terminal\n");
		return false;
	}

	if (!tty_register(g_terminal.tty, TERMIOS_TEST_DEVICE, 0U, 0U, 0666U)) {
		kprintf("cannot register /dev/%s\n", TERMIOS_TEST_DEVICE);
		tty_release(g_terminal.tty);
		return false;
	}

	proc_t proc = 0;
	loader_status_t loaded =
		loader_spawn_as(proc_kernel(), TERMIOS_TEST_PATH, "ttytest", 0U, NXU_UID_NOBODY, NXU_GID_NOBODY, &proc);
	bool ok = loaded == LOADER_STATUS_OK;
	uint64_t status = 0ULL;

	if (!ok) kprintf("spawning ttytest failed: %s\n", loader_status_name(loaded));

	uint64_t deadline = timer_get_microseconds() + TERMIOS_TEST_TIMEOUT_US;

	while (ok && !proc_reap(proc_kernel(), proc->p_ident.pid, &status)) {
		if (timer_get_microseconds() > deadline || !sched_yield()) {
			kprintf("ttytest did not exit in time\n");
			ok = false;
		}
	}

	if (ok && status != 0ULL) {
		kprintf("ttytest failed with status 0x%llx (check number, or a kill)\n", (unsigned long long)status);
		ok = false;
	}

	tty_unregister(g_terminal.tty);
	tty_detach(g_terminal.tty);
	tty_release(g_terminal.tty);

	if (ok && (!g_terminal.released || tty_count() != terminals)) {
		kprintf("the terminal was not freed: a file on it stayed open\n");
		ok = false;
	}

	if (ok) kprintf("programs configure their terminal with termios and stty\n");

	return ok;
}
