/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ioready.h
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

#ifndef NXU_KERN_IOREADY_H
#define NXU_KERN_IOREADY_H

#include <kern/lock.h>

#include <stdbool.h>
#include <stdint.h>

/* What an object can be ready for. Used both for levels and for edges. */
#define IOREADY_READ (1U << 0U)	  /* a read would not wait: data, or end of stream */
#define IOREADY_WRITE (1U << 1U)  /* a write would not wait */
#define IOREADY_HANGUP (1U << 2U) /* the other side is gone; never masked by a watch */
#define IOREADY_ERROR (1U << 3U)  /* the object is in an error state; never masked by a watch */

struct ioready;
struct ioready_watch;

typedef void (*ioready_notify_t)(struct ioready_watch *watch, uint32_t events);

/*
 * ioready_watch_t
 *
 * One listener on one ioready_t.
 *
 * events
 *     What the watcher asked to hear about. IOREADY_HANGUP and IOREADY_ERROR
 *     are delivered whatever it asked for.
 *
 * fired
 *     Every event delivered to this watch since it was attached, accumulated.
 *     Read it with ioready_watch_fired.
 *
 * notify / context
 *     Called for each delivery, under the rules above; may be null for a
 *     watcher that only reads `fired`. context is the watcher's own.
 */
typedef struct ioready_watch {
	struct ioready_watch *next;
	struct ioready_watch *prev;
	struct ioready *source;
	uint32_t events;
	uint32_t fired;
	ioready_notify_t notify;
	void *context;
} ioready_watch_t;

typedef struct ioready {
	nxu_spinlock_t lock;
	ioready_watch_t *watches;
} ioready_t;

/* An all-zero ioready_t is valid and has no watches; this is for readability. */
void ioready_init(ioready_t *ready);

/*
 * ioready_post
 *
 * The object may have become ready for `events`. Delivers to every watch
 * that asked for any of them and wakes the event-wait sleepers.
 */
void ioready_post(ioready_t *ready, uint32_t events);

/*
 * ioready_destroy
 *
 * The object is going away. Every watch still attached is detached and told
 * IOREADY_HANGUP. After this returns nothing refers to `ready`.
 */
void ioready_destroy(ioready_t *ready);

/*
 * ioready_attach / ioready_detach
 *
 * Start and stop listening. A watch is attached to at most one ioready_t.
 * Detaching a watch that is not attached (never was, or was detached by
 * ioready_destroy) does nothing, so a watcher can always detach on its way
 * out. No delivery to the watch is in progress or can begin once
 * ioready_detach has returned.
 */
void ioready_attach(ioready_t *ready, ioready_watch_t *watch, uint32_t events, ioready_notify_t notify, void *context);
void ioready_detach(ioready_watch_t *watch);

/* The events delivered to watch so far. */
uint32_t ioready_watch_fired(const ioready_watch_t *watch);

#endif
