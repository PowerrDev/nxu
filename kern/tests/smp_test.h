/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/smp_test.h
 *
 * SMP validation: every CPU comes up, runs its own threads out of its own run
 * queue, is woken by IPIs, honours affinity, balances load, and shares the
 * kernel's memory and console safely. Run on a machine with at least two CPUs
 * (`make test TEST=smp`, QEMU -smp 4).
 */

#ifndef NXU_KERN_TESTS_SMP_TEST_H
#define NXU_KERN_TESTS_SMP_TEST_H

#include <stdbool.h>

bool smp_test_run(void);

/*
 * The cross-CPU VM subset only (shared-page coherence, TLB shootdown, fault vs.
 * munmap, COW race): for a port whose timer/scheduler model does not yet match
 * the full suite's assumptions but whose pmap TLB shootdown is to be validated.
 */
bool smp_test_run_vm(void);

#endif
