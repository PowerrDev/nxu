/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_kmsg.c
 *
 * Allocation and destruction of kernel-resident NXPC messages.
 */

#include <kern/ipc/ipc_kmsg.h>

#include <mach/machine/machine_routines.h>
#include <kern/ipc/ipc_port.h>
#include <kern/memory/heap.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint64_t g_kmsg_outstanding_count;
static uint64_t g_kmsg_outstanding_bytes;

/*
 * Routine:     ipc_kmsg_init
 * Purpose:
 *              Initialize kernel-message accounting before any ports become
 *              visible to the rest of NXU.
 */
void
ipc_kmsg_init(void)
{
	g_kmsg_outstanding_count = 0ULL;
	g_kmsg_outstanding_bytes = 0ULL;
}

/*
 * Routine:     ipc_kmsg_alloc
 * Purpose:
 *              Copy one bounded inline payload into kernel-owned storage.
 *              The returned object has no queue owner until it is enqueued.
 *              If xfer_port is not IPC_PORT_NULL, the message takes over the
 *              caller's reference on it (the caller must already own one).
 */
ipc_return_t
ipc_kmsg_alloc(const void *data, size_t size, ipc_port_t xfer_port, ipc_kmsg_t *kmsgp)
{
	if (data == 0 || kmsgp == 0 || size == 0U) {
		return IPC_INVALID_ARGUMENT;
	}

	if (size > IPC_KMSG_MAX_INLINE_SIZE) {
		return IPC_MESSAGE_TOO_LARGE;
	}

	if (size > (size_t)-1 - sizeof(struct ipc_kmsg)) {
		return IPC_OVERFLOW;
	}

	ipc_kmsg_t kmsg = kmalloc(sizeof(struct ipc_kmsg) + size);
	if (kmsg == IPC_KMSG_NULL) return IPC_NO_MEMORY;

	kmsg->ikm_next = IPC_KMSG_NULL;
	kmsg->ikm_size = size;
	kmsg->ikm_flags = 0U;
	kmsg->ikm_xfer_type = xfer_port != IPC_PORT_NULL ? IPC_KMSG_XFER_PORT : IPC_KMSG_XFER_NONE;
	kmsg->ikm_xfer_port = xfer_port;
	memcpy(kmsg->ikm_data, data, size);

	uint64_t irq_state = ml_irq_save();
	g_kmsg_outstanding_count++;
	g_kmsg_outstanding_bytes += (uint64_t)size;
	ml_irq_restore(irq_state);

	*kmsgp = kmsg;
	return IPC_SUCCESS;
}

/*
 * Routine:     ipc_kmsg_free
 * Purpose:
 *              Release a message after it has left every port queue. Port
 *              destruction uses the same path while draining queued data.
 *              Releases the message's own reference on a transferred port
 *              that was never received (dropped with the port still active,
 *              or the destination port destroyed while still queued).
 */
void
ipc_kmsg_free(ipc_kmsg_t kmsg)
{
	if (kmsg == IPC_KMSG_NULL) return;

	if (kmsg->ikm_xfer_type == IPC_KMSG_XFER_PORT) {
		ipc_port_release(kmsg->ikm_xfer_port);
		kmsg->ikm_xfer_type = IPC_KMSG_XFER_NONE;
		kmsg->ikm_xfer_port = IPC_PORT_NULL;
	}

	uint64_t irq_state = ml_irq_save();

	if (g_kmsg_outstanding_count != 0ULL) {
		g_kmsg_outstanding_count--;
	}

	if (g_kmsg_outstanding_bytes >= (uint64_t)kmsg->ikm_size) {
		g_kmsg_outstanding_bytes -= (uint64_t)kmsg->ikm_size;
	} else {
		g_kmsg_outstanding_bytes = 0ULL;
	}

	ml_irq_restore(irq_state);
	(void)kfree(kmsg);
}

uint64_t
ipc_kmsg_outstanding_count(void)
{
	uint64_t irq_state = ml_irq_save();
	uint64_t count = g_kmsg_outstanding_count;
	ml_irq_restore(irq_state);
	return count;
}

uint64_t
ipc_kmsg_outstanding_bytes(void)
{
	uint64_t irq_state = ml_irq_save();
	uint64_t bytes = g_kmsg_outstanding_bytes;
	ml_irq_restore(irq_state);
	return bytes;
}
