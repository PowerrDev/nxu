/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/boot_test.h
 *
 * Boot-time test modes. A test build (make test TEST=<id>, see
 * makedefs/tests.mk) selects one with a -DNXU_*_TEST switch; the kernel
 * then boots into that test instead of the normal userspace and halts when
 * it finishes. A normal build compiles both hooks to nothing.
 */

#ifndef NXU_KERN_TESTS_BOOT_TEST_H
#define NXU_KERN_TESTS_BOOT_TEST_H

#include <drivers/video/display.h>

/*
 * boot_test_graphical
 *
 * The WindowServer / UIService bring-up tests. They need only the drivers,
 * IPC and the scheduler, and skip the filesystem entirely. Never returns if
 * one is selected; returns immediately otherwise.
 */
void boot_test_graphical(display_device_t *boot_display);

/*
 * boot_test_storage
 *
 * The tests that need /disk mounted: the Btrfs boot-arg test, the JBD2
 * crash test and every userland process test. Never returns if one is
 * selected; returns immediately otherwise.
 */
void boot_test_storage(display_device_t *boot_display);

/*
 * boot_test_unified
 *
 * The unified boot (-DNXU_UNIFIED_BOOT_TEST) or the plain desktop boot's UI
 * session (-DNXU_DESKTOP_BOOT, the same session without the test thread) --
 * see kern/tests/unified_boot.h. Called once the periodic timer runs and
 * userspace is about to be dispatched; it starts the test thread (unified
 * only) and then runs the UI session on the calling thread. Returns
 * immediately unless one of those is selected (and after it has said so if
 * the session ended).
 */
void boot_test_unified(void);

#endif
