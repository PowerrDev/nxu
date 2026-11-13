/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/tty_test.h
 *
 * Declares the terminal core test (kern/tests/tty_test.c).
 */
#ifndef NXU_KERN_TESTS_TTY_TEST_H
#define NXU_KERN_TESTS_TTY_TEST_H

#include <stdbool.h>

/*
 * Drive a terminal with a loopback driver through /dev and the VFS, with
 * kernel threads for the parts that sleep. Needs devfs mounted at /dev,
 * tty_init done and the scheduler running. Prints one line per failure
 * ("tty_test_run: FAILED: ..."); the caller reports success.
 */
bool tty_test_run(void);

#endif
