/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/pty_test.h
 *
 * Pseudo-terminal pairs, driven end to end through /dev/ptmx and the VFS.
 */
#ifndef NXU_KERN_TESTS_PTY_TEST_H
#define NXU_KERN_TESTS_PTY_TEST_H

#include <stdbool.h>

/*
 * Make pairs through /dev/ptmx and use both sides the way a terminal
 * emulator and a shell would, with kernel threads for the parts that sleep.
 * Needs devfs mounted at /dev, tty_init done and the scheduler running.
 * Prints one line per failure ("pty_test_run: FAILED: ..."); the caller
 * reports success.
 */
bool pty_test_run(void);

#endif
