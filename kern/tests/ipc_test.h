/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_test.h
 *
 * NXPC kernel transport bootstrap validation.
 */

#ifndef NXU_KERN_IPC_TEST_H
#define NXU_KERN_IPC_TEST_H

#include <stdbool.h>

bool ipc_self_test(void);
bool ipc_space_self_test(void);

#endif
