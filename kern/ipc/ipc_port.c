/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_port.c
 *
 * NXPC message ports. A port is a reference-counted kernel object with one
 * bounded FIFO of kernel messages.
 */

#include <kern/ipc/ipc_port.h>

#include <kern/lock.h>
#include <kern/machine/machine_routines.h>
#include <kern/memory/heap.h>
#include <kern/sched_prism/waitq.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct ipc_port {
	ipc_object_id_t ip_object_id;
	uint32_t ip_references;
	uint32_t ip_qlimit;
	uint32_t ip_qlen;
	bool ip_active;

	ipc_kmsg_t ip_messages_head;
	ipc_kmsg_t ip_messages_tail;

	/* Protects everything above (references, queue, active flag). */
	nxu_spinlock_t ip_lock;

	/* Threads sleeping in ipc_port_wait for a message. */
	waitq_t ip_waiters;
};

/* Diagnostics only: updated with atomics, no lock. */
static ipc_object_id_t g_next_port_object_id;
static uint64_t g_port_outstanding_count;

/*
 * One spinlock per port, taken with interrupts masked. It protects the port's
 * reference count, active flag and message queue against senders, receivers and
 * releasers on any CPU. It is a leaf: nothing is called while it is held (queued
 * messages are freed after it is dropped, and wakeups are made after it is
 * dropped too, so it is never held together with a scheduler lock).
 */
static uint64_t
ipc_port_lock(ipc_port_t port)
{
	return nxu_spin_lock_irqsave(&port->ip_lock);
}

static void
ipc_port_unlock(ipc_port_t port, uint64_t state)
{
	nxu_spin_unlock_irqrestore(&port->ip_lock, state);
}

/*
 * Routine:     ipc_port_init
 * Purpose:
 *              Initialize the kernel port-object namespace. Object IDs are
 *              diagnostics only; userspace will later receive per-task names
 *              through ipc_space rather than these global identifiers.
 */
void
ipc_port_init(void)
{
	g_next_port_object_id = 1ULL;
	g_port_outstanding_count = 0ULL;
}

/*
 * Routine:     ipc_port_alloc
 * Purpose:
 *              Create one active receive endpoint with an initial reference.
 */
ipc_return_t
ipc_port_alloc(ipc_port_t *portp)
{
	if (portp == 0) return IPC_INVALID_ARGUMENT;

	ipc_port_t port = kcalloc(1U, sizeof(struct ipc_port));
	if (port == IPC_PORT_NULL) return IPC_NO_MEMORY;

	ipc_object_id_t object_id = __atomic_fetch_add(&g_next_port_object_id, 1U, __ATOMIC_RELAXED);

	if (object_id == IPC_OBJECT_ID_NULL) {
		(void)kfree(port);
		return IPC_OVERFLOW;
	}

	(void)__atomic_add_fetch(&g_port_outstanding_count, 1ULL, __ATOMIC_RELAXED);

	port->ip_object_id = object_id;
	port->ip_references = 1U;
	port->ip_qlimit = IPC_PORT_QUEUE_LIMIT;
	port->ip_qlen = 0U;
	port->ip_active = true;
	port->ip_messages_head = IPC_KMSG_NULL;
	port->ip_messages_tail = IPC_KMSG_NULL;

	*portp = port;
	return IPC_SUCCESS;
}

/*
 * Routine:     ipc_port_reference
 * Purpose:
 *              Acquire another lifetime reference while the port remains
 *              active. Callers must hold a reference across queue operations.
 */
bool
ipc_port_reference(ipc_port_t port)
{
	if (port == IPC_PORT_NULL) return false;

	uint64_t state = ipc_port_lock(port);

	if (!port->ip_active || port->ip_references == UINT32_MAX) {
		ipc_port_unlock(port, state);
		return false;
	}

	port->ip_references++;
	ipc_port_unlock(port, state);
	return true;
}

/*
 * Routine:     ipc_port_release
 * Purpose:
 *              Drop one lifetime reference. The final release deactivates the
 *              endpoint, detaches its FIFO atomically, then destroys queued
 *              messages outside the IRQ-masked section.
 */
void
ipc_port_release(ipc_port_t port)
{
	if (port == IPC_PORT_NULL) return;

	ipc_kmsg_t messages = IPC_KMSG_NULL;
	bool destroy = false;
	uint64_t state = ipc_port_lock(port);

	if (port->ip_references != 0U) {
		port->ip_references--;
	}

	if (port->ip_references == 0U && port->ip_active) {
		port->ip_active = false;
		messages = port->ip_messages_head;
		port->ip_messages_head = IPC_KMSG_NULL;
		port->ip_messages_tail = IPC_KMSG_NULL;
		port->ip_qlen = 0U;
		(void)__atomic_sub_fetch(&g_port_outstanding_count, 1ULL, __ATOMIC_RELAXED);
		destroy = true;
	}

	ipc_port_unlock(port, state);

	if (!destroy) return;

	while (messages != IPC_KMSG_NULL) {
		ipc_kmsg_t next = messages->ikm_next;
		messages->ikm_next = IPC_KMSG_NULL;
		ipc_kmsg_free(messages);
		messages = next;
	}

	(void)kfree(port);
}

/*
 * Routine:     ipc_port_enqueue
 * Purpose:
 *              Append one kernel-owned message to the FIFO. Ownership moves
 *              to the port only when IPC_SUCCESS is returned.
 */
ipc_return_t
ipc_port_enqueue(ipc_port_t port, ipc_kmsg_t kmsg)
{
	if (port == IPC_PORT_NULL || kmsg == IPC_KMSG_NULL) return IPC_INVALID_ARGUMENT;

	uint64_t state = ipc_port_lock(port);

	if (!port->ip_active) {
		ipc_port_unlock(port, state);
		return IPC_PORT_INACTIVE;
	}

	if (port->ip_qlen >= port->ip_qlimit) {
		ipc_port_unlock(port, state);
		return IPC_QUEUE_FULL;
	}

	kmsg->ikm_next = IPC_KMSG_NULL;

	if (port->ip_messages_tail != IPC_KMSG_NULL) {
		port->ip_messages_tail->ikm_next = kmsg;
	} else {
		port->ip_messages_head = kmsg;
	}

	port->ip_messages_tail = kmsg;
	port->ip_qlen++;
	ipc_port_unlock(port, state);

	/* A receiver may be asleep waiting for exactly this. */
	waitq_wake_one(&port->ip_waiters);

	return IPC_SUCCESS;
}

/*
 * Routine:     ipc_port_wait
 * Purpose:
 *              Sleep until a message may have arrived on port. The caller
 *              holds a reference (so the port cannot disappear under the
 *              sleeper) and retries ipc_port_dequeue afterwards. Returns
 *              false when a signal interrupted the wait.
 */
bool
ipc_port_wait(ipc_port_t port)
{
	if (port == IPC_PORT_NULL) return false;

	return waitq_block(&port->ip_waiters, true);
}

/*
 * Routine:     ipc_port_wait_seq / ipc_port_wait_prepare
 * Purpose:
 *              The receive loop without a lost wakeup: take the sequence
 *              number with ipc_port_wait_prepare BEFORE calling
 *              ipc_port_dequeue, and sleep with ipc_port_wait_seq only if the
 *              dequeue found nothing. A message enqueued between the two makes
 *              the sequence move (ipc_port_enqueue wakes the waiters) and the
 *              sleep returns at once instead of missing it.
 */
uint32_t
ipc_port_wait_prepare(ipc_port_t port)
{
	return port == IPC_PORT_NULL ? 0U : waitq_seq(&port->ip_waiters);
}

bool
ipc_port_wait_seq(ipc_port_t port, uint32_t seq)
{
	if (port == IPC_PORT_NULL) return false;

	return waitq_block_seq(&port->ip_waiters, seq, true);
}

/*
 * Routine:     ipc_port_dequeue
 * Purpose:
 *              Remove the oldest message from the FIFO. The caller owns the
 *              returned kernel message and must eventually free or forward it.
 */
ipc_return_t
ipc_port_dequeue(ipc_port_t port, ipc_kmsg_t *kmsgp)
{
	if (port == IPC_PORT_NULL || kmsgp == 0) return IPC_INVALID_ARGUMENT;

	uint64_t state = ipc_port_lock(port);

	if (!port->ip_active) {
		ipc_port_unlock(port, state);
		return IPC_PORT_INACTIVE;
	}

	ipc_kmsg_t kmsg = port->ip_messages_head;
	if (kmsg == IPC_KMSG_NULL) {
		ipc_port_unlock(port, state);
		return IPC_QUEUE_EMPTY;
	}

	port->ip_messages_head = kmsg->ikm_next;

	if (port->ip_messages_head == IPC_KMSG_NULL) {
		port->ip_messages_tail = IPC_KMSG_NULL;
	}

	port->ip_qlen--;
	kmsg->ikm_next = IPC_KMSG_NULL;
	ipc_port_unlock(port, state);

	*kmsgp = kmsg;
	return IPC_SUCCESS;
}

ipc_object_id_t
ipc_port_object_id(ipc_port_t port)
{
	if (port == IPC_PORT_NULL) return IPC_OBJECT_ID_NULL;
	return port->ip_object_id;
}

uint32_t
ipc_port_qlen(ipc_port_t port)
{
	if (port == IPC_PORT_NULL) return 0U;

	uint64_t state = ipc_port_lock(port);
	uint32_t qlen = port->ip_qlen;
	ipc_port_unlock(port, state);
	return qlen;
}

uint32_t
ipc_port_qlimit(ipc_port_t port)
{
	if (port == IPC_PORT_NULL) return 0U;
	return port->ip_qlimit;
}

bool
ipc_port_active(ipc_port_t port)
{
	if (port == IPC_PORT_NULL) return false;

	uint64_t state = ipc_port_lock(port);
	bool active = port->ip_active;
	ipc_port_unlock(port, state);
	return active;
}

uint64_t
ipc_port_outstanding_count(void)
{
	return __atomic_load_n(&g_port_outstanding_count, __ATOMIC_RELAXED);
}
