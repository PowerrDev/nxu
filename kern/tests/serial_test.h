/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/serial_test.h
 *
 * Declares the serial terminal test (kern/tests/serial_test.c).
 */
#ifndef NXU_KERN_TESTS_SERIAL_TEST_H
#define NXU_KERN_TESTS_SERIAL_TEST_H

#include <stdbool.h>

/*
 * Drive /dev/console from the other end of the wire: the test prints
 * "serial_test_run: ready for <step>" and waits for the host to type what
 * that step needs into the console UART (tools/serial_expect.pl, with the
 * script tools/serial_test.script). Needs serial_init done and the
 * scheduler running. Prints one line per failure ("serial_test_run:
 * FAILED: ..."); the caller reports success.
 */
bool serial_test_run(void);

#endif
