/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/process_control_test.h
 *
 * fork / exec / signals / copy-on-write / demand-paging validation.
 */

#ifndef NXU_KERN_TESTS_PROCESS_CONTROL_TEST_H
#define NXU_KERN_TESTS_PROCESS_CONTROL_TEST_H

#include <stdbool.h>

bool process_control_test(void);

/*
 * The same test for a boot that runs other work at the same time: everything
 * proctest and privtest check still holds, but the check that every page was
 * given back is skipped, since the count of pages in use is not the test's
 * alone there.
 */
bool process_control_test_shared(void);

#endif
