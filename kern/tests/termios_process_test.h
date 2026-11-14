/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/termios_process_test.h
 *
 * Declares the kernel-side drivers of the ttytest and restarttest user programs.
 */
#ifndef NXU_KERN_TESTS_TERMIOS_PROCESS_TEST_H
#define NXU_KERN_TESTS_TERMIOS_PROCESS_TEST_H

#include <stdbool.h>

/*
 * Put a loopback terminal at /dev/ttytest, run ttytest on it as an ordinary
 * user, and report whether every check passed. Needs the system volume
 * mounted, tty_init done and the scheduler running.
 */
bool termios_process_test(void);

/* Run restarttest on a loopback terminal at /dev/ttyrestart the same way. */
bool restart_process_test(void);

/* Run ptytest, which makes its own pseudo-terminal and drives a shell on it, the same way. */
bool pty_process_test(void);

#endif
