/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_port.h
 *
 * NXPC message-port lifecycle and queue operations.
 */

#ifndef NXU_KERN_IPC_PORT_H
#define NXU_KERN_IPC_PORT_H

#include <kern/ipc/ipc_kmsg.h>
#include <kern/ipc/ipc_types.h>

#include <stdbool.h>
#include <stdint.h>

void ipc_port_init(void);

ipc_return_t ipc_port_alloc(ipc_port_t *portp);
bool ipc_port_reference(ipc_port_t port);
void ipc_port_release(ipc_port_t port);

ipc_return_t ipc_port_enqueue(ipc_port_t port, ipc_kmsg_t kmsg);
ipc_return_t ipc_port_dequeue(ipc_port_t port, ipc_kmsg_t *kmsgp);

/*
 * Sleep until a message may have arrived. The caller must hold a reference
 * to port and retry ipc_port_dequeue afterwards. False means a signal
 * interrupted the wait.
 */
bool ipc_port_wait(ipc_port_t port);

ipc_object_id_t ipc_port_object_id(ipc_port_t port);
uint32_t ipc_port_qlen(ipc_port_t port);
uint32_t ipc_port_qlimit(ipc_port_t port);
bool ipc_port_active(ipc_port_t port);
uint64_t ipc_port_outstanding_count(void);

#endif
