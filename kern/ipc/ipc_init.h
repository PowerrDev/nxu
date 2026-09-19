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

#include <kern/ipc/ipc_types.h>

#include <stdbool.h>

void ipc_init(void);
bool ipc_initialized(void);

/*
 * The system bootstrap registry port: one well-known port, owned by bootd
 * (PID 1), that every other process inherits a send right to at spawn (see
 * loader_spawn) so it can register/look up named services without a side
 * channel. IPC_PORT_NULL until bootd calls ipc_set_bootstrap_registry_port
 * (restricted to PID 1 at the syscall layer).
 */
ipc_port_t ipc_bootstrap_registry_port(void);

/* Takes ownership of the caller's reference on port, releasing any previous one. */
void ipc_set_bootstrap_registry_port(ipc_port_t port);

#endif
