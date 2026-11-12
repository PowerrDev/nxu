/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/spawn_process_test.h
 *
 * Declares the kernel-side driver of the spawntest user program.
 */
#ifndef NXU_KERN_TESTS_SPAWN_PROCESS_TEST_H
#define NXU_KERN_TESTS_SPAWN_PROCESS_TEST_H

#include <stdbool.h>

/* Run spawntest as an ordinary user and report whether every check in it passed. */
bool spawn_process_test(void);

#endif
