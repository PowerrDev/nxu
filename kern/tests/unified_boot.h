/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/unified_boot.h
 *
 * The unified boot (make run, make test TEST=everything): one kernel and one
 * QEMU instance in which everything that can share a boot is running at once.
 * It is a normal boot -- bootd as PID 1 with its daemons, the boot chime --
 * with two additions:
 *
 *   - the UIService session (WindowServer + the Voyager app) runs on the
 *     boot thread and owns the display, as it does in the ui-voyager test
 *   - a kernel thread runs the userland tests one after another against that
 *     live system (bootd's registry, the scheduler shared with the UI and the
 *     chime) and prints one summary block at the end
 *
 * What cannot share a boot, and why, is listed in makedefs/tests.mk and
 * doc/testing.md: a test that halts the kernel, one that provokes crashes on
 * purpose, and the ones that each need to be the display server.
 *
 * A normal build compiles the two hooks that call this to nothing.
 */

#ifndef NXU_KERN_TESTS_UNIFIED_BOOT_H
#define NXU_KERN_TESTS_UNIFIED_BOOT_H

/*
 * unified_boot_prepare:
 *
 * With the system volume mounted and bootd not yet started: turn off the
 * bootd services that would each claim the display the UI session owns.
 */
void unified_boot_prepare(void);

/*
 * unified_boot_run:
 *
 * Once userspace is being dispatched: start the thread that runs the tests,
 * then run the UI session on the calling (boot) thread. Returns only if the
 * session could not start or ended, after saying so; the caller then carries
 * on as the plain scheduler loop.
 */
void unified_boot_run(void);

/*
 * unified_boot_run_ui_only:
 *
 * unified_boot_run() without the test thread: just the UI session (WindowServer
 * + the Voyager app) on the calling (boot) thread, for a normal boot that wants
 * Voyager but not a live regression run sharing its CPU (make run, NXU_DESKTOP_BOOT
 * -- see makedefs/tests.mk's `desktop` test id). Same return convention as
 * unified_boot_run(). unified_boot_prepare() must still run first, same as for
 * unified_boot_run(), so bootd does not also start its own display server.
 */
void unified_boot_run_ui_only(void);

#endif
