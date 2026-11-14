/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/termios_process_test.c
 *
 * Runs user programs that test the terminal and the signals that reach it,
 * each on a terminal of its own, and waits for their verdicts:
 *
 *   ttytest      <termios.h> and stty (frameworks/BootDaemons.framework/
 *                ttytest.c, stty.c), on /dev/ttytest
 *   restarttest  system calls interrupted by signals, restarted or not
 *                (restarttest.c), on /dev/ttyrestart
 *   ptytest      a shell on a pseudo-terminal, driven from the master as
 *                Terminal.app drives it (ptytest.c); it makes its own
 *                terminal, so it gets no loopback one
 *
 * The terminal is the loopback test driver (kern/tests/tty_loopback.h), open
 * to everyone: the programs run as an ordinary user holding no capabilities,
 * because nothing a program does to its own terminal needs any. When a program
 * is done its terminal is taken down the way a driver that goes away does
 * (unregister, detach, release), and it must then be freed: every file the
 * program and its children opened on it has been closed by their exits.
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
#include <string.h>

#define CORESERVICES "/Volumes/mainOS/System/Library/CoreServices/"
#define PROGRAM_TEST_TIMEOUT_US 60000000ULL

static tty_loopback_t g_terminal;

/*
 * Put a loopback terminal at /dev/`device`, open to everyone, run the program
 * `name` from CoreServices on it as an ordinary user, and take the terminal
 * down again the way a driver that goes away does. True when the program
 * exited 0 and the terminal was then freed (every file on it closed). With
 * no `device`, the program runs without one, and every terminal it made must
 * be gone once it has exited.
 */
static bool
run_on_terminal(const char *name, const char *device)
{
	char path[96];
	uint32_t terminals = tty_count();
	size_t prefix = sizeof(CORESERVICES) - 1U;
	size_t length = strlen(name);

	if (prefix + length + 1U > sizeof(path)) return false;

	memcpy(path, CORESERVICES, prefix);
	memcpy(path + prefix, name, length + 1U);

	if (device == 0) {
		g_terminal.tty = TTY_NULL;
	} else if (!tty_loopback_create(&g_terminal)) {
		kprintf("cannot create the terminal for %s\n", name);
		return false;
	}

	if (device != 0 && !tty_register(g_terminal.tty, device, 0U, 0U, 0666U)) {
		kprintf("cannot register /dev/%s\n", device);
		tty_release(g_terminal.tty);
		return false;
	}

	proc_t proc = 0;
	loader_status_t loaded = loader_spawn_as(proc_kernel(), path, name, 0U, NXU_UID_NOBODY, NXU_GID_NOBODY, &proc);
	bool ok = loaded == LOADER_STATUS_OK;
	uint64_t status = 0ULL;

	if (!ok) kprintf("spawning %s failed: %s\n", name, loader_status_name(loaded));

	uint64_t deadline = timer_get_microseconds() + PROGRAM_TEST_TIMEOUT_US;

	while (ok && !proc_reap(proc_kernel(), proc->p_ident.pid, &status)) {
		if (timer_get_microseconds() > deadline || !sched_yield()) {
			kprintf("%s did not exit in time\n", name);
			ok = false;
		}
	}

	if (ok && status != 0ULL) {
		kprintf("%s failed with status 0x%llx (check number, or a kill)\n", name, (unsigned long long)status);
		ok = false;
	}

	if (device != 0) {
		tty_unregister(g_terminal.tty);
		tty_detach(g_terminal.tty);
		tty_release(g_terminal.tty);
	}

	if (ok && ((device != 0 && !g_terminal.released) || tty_count() != terminals)) {
		kprintf("the terminal of %s was not freed: a file on it stayed open\n", name);
		ok = false;
	}

	return ok;
}

bool
termios_process_test(void)
{
	if (!run_on_terminal("ttytest", "ttytest")) return false;

	kprintf("programs configure their terminal with termios and stty\n");
	return true;
}

bool
restart_process_test(void)
{
	if (!run_on_terminal("restarttest", "ttyrestart")) return false;

	kprintf("interrupted system calls are restarted, or not, as SA_RESTART and the call say\n");
	return true;
}

bool
pty_process_test(void)
{
	if (!run_on_terminal("ptytest", 0)) return false;

	kprintf("a shell on a pseudo-terminal edits, runs, interrupts, stops and continues jobs\n");
	return true;
}
