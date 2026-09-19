/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_kmsg.h
 *
 * Kernel-resident NXPC message objects.
 */

#ifndef NXU_KERN_IPC_KMSG_H
#define NXU_KERN_IPC_KMSG_H

#include <kern/ipc/ipc_types.h>

#include <stddef.h>
#include <stdint.h>

/*
 * A kernel message owns its payload. Senders never leave pointers to their
 * storage in a port queue; data is copied into this object before enqueue.
 *
 * ikm_xfer_port holds one lifetime reference on behalf of the message
 * (taken over from the caller at ipc_kmsg_alloc time, released by
 * ipc_kmsg_free) whenever ikm_xfer_type is IPC_KMSG_XFER_PORT. Whoever
 * receives the message and moves ikm_xfer_port into their own ipc_space
 * must clear ikm_xfer_type back to IPC_KMSG_XFER_NONE first, so the
 * ownership handoff happens exactly once -- see ipc_space_insert_port.
 */
struct ipc_kmsg {
	ipc_kmsg_t ikm_next;
	size_t ikm_size;
	uint32_t ikm_flags;
	ipc_kmsg_xfer_type_t ikm_xfer_type;
	ipc_port_t ikm_xfer_port;
	uint8_t ikm_data[];
};

void ipc_kmsg_init(void);

/*
 * xfer_port, when not IPC_PORT_NULL, must already be a reference the caller
 * owns and is handing off to the message (mirroring how file_alloc hands
 * filedesc_install an already-referenced file_t). Passing IPC_PORT_NULL
 * allocates a message with no transfer, exactly as before this parameter
 * existed.
 */
ipc_return_t ipc_kmsg_alloc(const void *data, size_t size, ipc_port_t xfer_port, ipc_kmsg_t *kmsgp);
void ipc_kmsg_free(ipc_kmsg_t kmsg);

uint64_t ipc_kmsg_outstanding_count(void);
uint64_t ipc_kmsg_outstanding_bytes(void);

#endif
