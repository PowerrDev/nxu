/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_init.h
 *
 * NXPC kernel transport initialization.
 */

#ifndef NXU_KERN_IPC_INIT_H
#define NXU_KERN_IPC_INIT_H

#include <stdbool.h>

void ipc_init(void);
bool ipc_initialized(void);

#endif
