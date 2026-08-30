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
 */
struct ipc_kmsg {
	ipc_kmsg_t ikm_next;
	size_t ikm_size;
	uint32_t ikm_flags;
	uint32_t ikm_reserved;
	uint8_t ikm_data[];
};

void ipc_kmsg_init(void);

ipc_return_t ipc_kmsg_alloc(const void *data, size_t size, ipc_kmsg_t *kmsgp);
void ipc_kmsg_free(ipc_kmsg_t kmsg);

uint64_t ipc_kmsg_outstanding_count(void);
uint64_t ipc_kmsg_outstanding_bytes(void);

#endif
