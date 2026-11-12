/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/signal_process_test.h
 *
 * Declares the kernel-side driver of the sigtest user program.
 */
#ifndef NXU_KERN_TESTS_SIGNAL_PROCESS_TEST_H
#define NXU_KERN_TESTS_SIGNAL_PROCESS_TEST_H

#include <stdbool.h>

/* Run sigtest as an ordinary user and report whether every check in it passed. */
bool signal_process_test(void);

#endif
