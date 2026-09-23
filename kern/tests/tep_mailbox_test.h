/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/tep_mailbox_test.h
 *
 * Exercises the Trusted Enclave mailbox against a live tepOS in a second
 * QEMU (tools/test_tep_mailbox.sh drives both machines):
 *
 *   1. protocol: HELLO, health, and tepOS's refusal of an unknown command
 *      and of a payload of the wrong length;
 *   2. fail closed: once the harness stops tepOS, the driver must declare it
 *      unavailable and refuse requests without touching the link;
 *   3. recovery: once the harness restarts tepOS, the driver reconnects.
 *
 * Each phase prints its own pass line so the harness knows when to act.
 */

#ifndef NXU_KERN_TESTS_TEP_MAILBOX_TEST_H
#define NXU_KERN_TESTS_TEP_MAILBOX_TEST_H

#include <stdbool.h>

bool tep_mailbox_test_run(void);

#endif
