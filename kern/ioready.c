/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ioready.c
 *
 * Readiness notification: how an object that can be read or written says
 * "look at me again", and how anything interested hears it.
 *
 * Readiness has two halves, and this file is the second.
 *
 *   - The level: is the object readable, writable, hung up right now? That
 *     is a question put to the object (the poll operation of a vnode or a
 *     device, socket_poll), answered without sleeping.
 *
 *   - The edge: the object has just become ready in some way it was not. The
 *     object owns an ioready_t and calls ioready_post on it. Whoever wants to
 *     know attaches an ioready_watch_t to that ioready_t; the object never
 *     learns who is watching, or whether anyone is.
 *
 * A waiter uses both, in this order, and the order is what makes it safe:
 * attach a watch, then ask for the level. A change that happened before the
 * attach shows in the level; one that happens after it is posted to the
 * watch. Nothing falls between.
 *
 * Model:
 *
 *   - An edge is a hint, never a promise. A post means "the level may have
 *     changed in this way", and the listener asks again; a post with nothing
 *     behind it is harmless. Producers therefore post generously and never
 *     need to know the previous state exactly.
 *
 *   - Every post also wakes the threads sleeping in the event-wait system
 *     call (sched_kick_sleepers), which is how a program that waits for
 *     window events learns that one of its descriptors has something for it.
 *     That is the only listener today. A poll or select system call needs no
 *     more than this file offers: one watch per descriptor, each with a
 *     notify routine that wakes the polling thread.
 *
 * Ownership:
 *
 *   - An ioready_t is embedded in the object it speaks for and lives exactly
 *     as long as that object. The object calls ioready_destroy before its
 *     memory goes.
 *
 *   - An ioready_watch_t belongs to the watcher (it is typically on the
 *     stack of a sleeping thread). The watcher detaches it before the watch's
 *     memory goes, and holds a reference to the watched object for as long as
 *     the watch is attached, so the ioready_t under the watch stays valid.
 *     ioready_destroy still detaches any watch it finds, telling it the object
 *     hung up, so that an object torn down by force leaves no watch pointing
 *     into freed memory.
 *
 * Locking:
 *
 *   - Each ioready_t has one spinlock, taken with interrupts masked, that
 *     protects its list of watches and each attached watch's link to it. It
 *     is a leaf with one exception: notify routines run under it.
 *
 *   - ioready_post may be called from any context, an interrupt handler
 *     included. The caller should have dropped the object's own lock first
 *     (the wakeup takes scheduler locks); it must never hold a lock that a
 *     notify routine could take.
 *
 *   - A notify routine runs with the ioready_t's lock held and interrupts
 *     masked. It may set a flag and wake a wait queue. It must not sleep,
 *     post, attach or detach, or call back into the object.
 */

#include <kern/ioready.h>

#include <kern/sched_prism/sched.h>
#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

/* Events a watch hears about whether it asked or not. */
#define IOREADY_ALWAYS (IOREADY_HANGUP | IOREADY_ERROR)

void
ioready_init(ioready_t *ready)
{
  kprintf("ioready_init: initializing readiness notification manager\n");
	if (ready == 0) return;

	ready->lock.value = 0U;
	ready->watches = 0;
}

static void
ioready_unlink_locked(ioready_t *ready, ioready_watch_t *watch)
{
	if (watch->prev != 0) {
		watch->prev->next = watch->next;
	} else {
		ready->watches = watch->next;
	}

	if (watch->next != 0) watch->next->prev = watch->prev;

	watch->next = 0;
	watch->prev = 0;

	/* Published last: ioready_detach finds its ioready_t through this word. */
	__atomic_store_n(&watch->source, (ioready_t *)0, __ATOMIC_RELEASE);
}

static void
ioready_deliver_locked(ioready_watch_t *watch, uint32_t events)
{
	uint32_t wanted = events & (watch->events | IOREADY_ALWAYS);

	if (wanted == 0U) return;

	(void)__atomic_or_fetch(&watch->fired, wanted, __ATOMIC_ACQ_REL);

	if (watch->notify != 0) watch->notify(watch, wanted);
}

void
ioready_post(ioready_t *ready, uint32_t events)
{
	if (ready == 0 || events == 0U) return;

	/* No watch, no lock: the common case today, and cheap enough for an interrupt handler. */
	if (__atomic_load_n(&ready->watches, __ATOMIC_ACQUIRE) != 0) {
		uint64_t irq_state = nxu_spin_lock_irqsave(&ready->lock);

		for (ioready_watch_t *watch = ready->watches; watch != 0; watch = watch->next)
			ioready_deliver_locked(watch, events);

		nxu_spin_unlock_irqrestore(&ready->lock, irq_state);
	}

	/*
	 * A watch attached after the test above missed nothing: it asks for the
	 * level after attaching, and the level already shows this change.
	 */
	sched_kick_sleepers();
}

void
ioready_destroy(ioready_t *ready)
{
	if (ready == 0) return;

	uint64_t irq_state = nxu_spin_lock_irqsave(&ready->lock);

	while (ready->watches != 0) {
		ioready_watch_t *watch = ready->watches;

		/* Delivered before the unlink, so the watcher's routine still sees an attached watch. */
		ioready_deliver_locked(watch, IOREADY_HANGUP);
		ioready_unlink_locked(ready, watch);
	}

	nxu_spin_unlock_irqrestore(&ready->lock, irq_state);

	sched_kick_sleepers();
}

void
ioready_attach(ioready_t *ready, ioready_watch_t *watch, uint32_t events, ioready_notify_t notify, void *context)
{
	if (ready == 0 || watch == 0) return;

	watch->events = events;
	watch->fired = 0U;
	watch->notify = notify;
	watch->context = context;

	uint64_t irq_state = nxu_spin_lock_irqsave(&ready->lock);

	watch->prev = 0;
	watch->next = ready->watches;

	if (ready->watches != 0) ready->watches->prev = watch;

	__atomic_store_n(&watch->source, ready, __ATOMIC_RELEASE);
	__atomic_store_n(&ready->watches, watch, __ATOMIC_RELEASE);

	nxu_spin_unlock_irqrestore(&ready->lock, irq_state);
}

void
ioready_detach(ioready_watch_t *watch)
{
  kprintf("ioready_detach: trying to detach watch\n");
	if (watch == 0) return;

	/*
	 * The ioready_t is found from the watch, and ioready_destroy on another
	 * CPU can detach the watch before this one has the lock; the check under
	 * the lock says whether the watch is still where it was. Taking the lock
	 * also waits out a delivery in progress, which is what lets the caller
	 * free the watch once this returns. The watcher's reference to the object
	 * keeps `ready` valid while this looks at it.
	 */
	for (;;) {
		ioready_t *ready = __atomic_load_n(&watch->source, __ATOMIC_ACQUIRE);

		if (ready == 0) return;

		uint64_t irq_state = nxu_spin_lock_irqsave(&ready->lock);

		if (watch->source != ready) {
			nxu_spin_unlock_irqrestore(&ready->lock, irq_state);
			continue;
		}

		ioready_unlink_locked(ready, watch);

		nxu_spin_unlock_irqrestore(&ready->lock, irq_state);
		return;
	}
}

uint32_t
ioready_watch_fired(const ioready_watch_t *watch)
{
	return watch == 0 ? 0U : __atomic_load_n(&watch->fired, __ATOMIC_ACQUIRE);
}
