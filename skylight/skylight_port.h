/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        skylight/skylight_port.h
 *
 * Portability shim for the Skylight compositor core (skylight/skylight.c). The
 * rendering code is pure software (surface pixels, blend/fill/copy/upload,
 * damage) and has no business knowing whose heap, lock, log or yield it runs on.
 * This header supplies those four primitives under the SL_* names skylight.c
 * uses, for three environments selected at compile time:
 *
 *   - kernel  (default)       kmalloc / nxu_spinlock / kprintf / sched_yield
 *   - userland (SL_USERLAND)  nxu_alloc / an inline spinlock / (silent) / relax
 *   - host    (SL_HOST)       malloc / an inline spinlock / fprintf / relax
 *
 * The kernel mappings are 1:1 aliases of the former direct calls, so the kernel
 * object is byte-identical to before the shim existed. Keep the primitives
 * minimal: this is the seam that lets Skylight become a userland framework
 * without the renderer changing.
 */
#ifndef SKYLIGHT_PORT_H
#define SKYLIGHT_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(SL_HOST)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SL_STRLEN(string) strlen(string)

typedef uint32_t sl_lock_t;
#define SL_LOCK_INIT 0U

static inline void
sl_lock(sl_lock_t *lock)
{
	while (__atomic_exchange_n(lock, 1U, __ATOMIC_ACQUIRE) != 0U)
		;
}

static inline void
sl_unlock(sl_lock_t *lock)
{
	__atomic_store_n(lock, 0U, __ATOMIC_RELEASE);
}

static inline void *
sl_zalloc(size_t count, size_t size)
{
	return calloc(count, size);
}

#define SL_ALLOC(size) malloc(size)
#define SL_FREE(pointer) free(pointer)
#define SL_LOG(...) ((void)fprintf(stderr, __VA_ARGS__))
#define SL_YIELD() (false)

#elif defined(SL_USERLAND)

#include <nxu/runtime.h>
#include <nxu/string.h>

#define SL_STRLEN(string) nxu_strlen(string)

/* A plain test-and-set spinlock: the userland window server composites on one
 * thread today, so this is contended only by the (future) present thread. */
typedef uint32_t sl_lock_t;
#define SL_LOCK_INIT 0U

static inline void
sl_lock(sl_lock_t *lock)
{
	while (__atomic_exchange_n(lock, 1U, __ATOMIC_ACQUIRE) != 0U)
		__asm__ volatile("" ::: "memory");
}

static inline void
sl_unlock(sl_lock_t *lock)
{
	__atomic_store_n(lock, 0U, __ATOMIC_RELEASE);
}

static inline void *
sl_zalloc(size_t count, size_t size)
{
	size_t bytes = count * size;
	void *pointer = nxu_alloc(bytes, 16U);

	if (pointer != 0) memset(pointer, 0, bytes);
	return pointer;
}

/* Pixel and struct allocations: 16-byte aligned so row copies stay aligned. */
#define SL_ALLOC(size) nxu_alloc((size), 16U)
#define SL_FREE(pointer) ((void)nxu_free(pointer))
/* Logging is routed by the hosting daemon later; silent in the library itself. */
#define SL_LOG(...) ((void)0)
#define SL_YIELD() (false)

#else /* kernel */

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/memory/heap.h>
#include <kern/sched_prism/sched.h>

#include <string.h>

#define SL_STRLEN(string) strlen(string)

typedef nxu_spinlock_t sl_lock_t;
#define SL_LOCK_INIT NXU_SPINLOCK_INIT

static inline void
sl_lock(sl_lock_t *lock)
{
	nxu_spin_lock(lock);
}

static inline void
sl_unlock(sl_lock_t *lock)
{
	nxu_spin_unlock(lock);
}

static inline void *
sl_zalloc(size_t count, size_t size)
{
	return kcalloc(count, size);
}

#define SL_ALLOC(size) kmalloc(size)
#define SL_FREE(pointer) ((void)kfree(pointer))
#define SL_LOG(...) kprintf(__VA_ARGS__)
#define SL_YIELD() sched_yield()

#endif

/* Common across environments: zero-filled allocation via each mode's sl_zalloc. */
#define SL_ZALLOC(count, size) sl_zalloc((count), (size))

#endif /* SKYLIGHT_PORT_H */
