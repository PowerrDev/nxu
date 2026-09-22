/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/smp_user_test.h
 *
 * SMP userland validation: spawns smptest (frameworks/BootDaemons.framework/
 * smptest.c), a user process that runs real EL0 processes and threads on
 * different CPUs at the same time and checks that they stay correct (address
 * space isolation across migration, concurrent page faults, shared memory, IPC,
 * killing a process whose threads run elsewhere, VM stress), then checks from the
 * kernel that several CPUs really did run user threads at once, that no run queue
 * invariant broke and that no memory leaked.
 */

#ifndef NXU_KERN_TESTS_SMP_USER_TEST_H
#define NXU_KERN_TESTS_SMP_USER_TEST_H

#include <stdbool.h>

bool smp_user_test_run(void);

#endif
