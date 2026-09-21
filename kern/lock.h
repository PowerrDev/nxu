#ifndef NXU_KERN_LOCK_H
#define NXU_KERN_LOCK_H

#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Canonical form of the atomic-exchange spinlock every subsystem in this
 * kernel (thread_lock, sched_lock, ipc_space's lock, ...) already hand-rolls
 * individually. New locks should use this type instead of copying the
 * pattern again; existing ones may migrate to it opportunistically.
 *
 * Memory ordering: taking the lock is an acquire and releasing it a release,
 * so everything the lock protects is ordered against it on ARM64's weak
 * memory model without a separate barrier. A lock does not order anything it
 * does not protect (device registers, page-table updates): those need the
 * architectural barriers their own rules call for.
 *
 * Use the irqsave forms for any lock an interrupt handler (or the timer tick)
 * can also take, or the CPU that holds it can be interrupted into a handler
 * that spins on it for ever. Never hold a spinlock across a sleep or a
 * scheduler switch (the scheduler's own runqueue lock is the one designed
 * exception; see kern/sched_prism/sched.c).
 */
typedef struct {
	volatile uint32_t value;
} nxu_spinlock_t;

#define NXU_SPINLOCK_INIT { 0U }

static inline void nxu_spin_lock(nxu_spinlock_t *lock)
{
	for (;;) {
		if (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) == 0U) return;

		/* Test-and-test-and-set: wait on a plain load, not on failed stores that bounce the line between CPUs. */
		while (__atomic_load_n(&lock->value, __ATOMIC_RELAXED) != 0U) cpu_relax();
	}
}

static inline bool nxu_spin_trylock(nxu_spinlock_t *lock)
{
	return __atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) == 0U;
}

static inline void nxu_spin_unlock(nxu_spinlock_t *lock)
{
	__atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

/* Mask interrupts on this CPU, then take the lock. Returns the state to give back to the unlock. */
static inline uint64_t nxu_spin_lock_irqsave(nxu_spinlock_t *lock)
{
	uint64_t state = ml_irq_save();

	nxu_spin_lock(lock);
	return state;
}

static inline void nxu_spin_unlock_irqrestore(nxu_spinlock_t *lock, uint64_t state)
{
	nxu_spin_unlock(lock);
	ml_irq_restore(state);
}

/*
 * Recursive spinlock, for the subsystems (page allocator, kernel VM, heap, page
 * tables, console) whose public entry points call each other and were written
 * as if nothing else could run. It is always taken with interrupts masked, so
 * the owner is a CPU, not a thread: the same CPU asking again is the owner
 * calling in from further down its own call chain, and only another CPU waits.
 *
 * Lock order between them, outermost first: heap -> vm_kern -> vmm -> pmm. The
 * console lock is a leaf that may be taken under any of them (they log).
 */
typedef struct {
	nxu_spinlock_t lock;
	uint32_t owner;		/* logical CPU id + 1, 0 when free */
	uint32_t depth;
} nxu_rlock_t;

#define NXU_RLOCK_INIT { NXU_SPINLOCK_INIT, 0U, 0U }

static inline uint64_t nxu_rlock_irqsave(nxu_rlock_t *rlock);
static inline void nxu_rlock_irqrestore(nxu_rlock_t *rlock, uint64_t state);

/*
 * Scope guard: takes the lock where declared and releases it on every way out
 * of the enclosing block (return, break, goto), so an entry point can be made
 * safe without touching each of its return paths.
 *
 *     bool pmm_allocate_page(...) {
 *         NXU_RLOCK_GUARD(&g_pmm_lock);
 *         ...
 */
typedef struct {
	nxu_rlock_t *rlock;
	uint64_t state;
} nxu_rlock_guard_t;

static inline void nxu_rlock_guard_release(nxu_rlock_guard_t *guard)
{
	nxu_rlock_irqrestore(guard->rlock, guard->state);
}

#define NXU_RLOCK_GUARD(rlock_pointer) \
	nxu_rlock_guard_t nxu_rlock_guard_ __attribute__((cleanup(nxu_rlock_guard_release), unused)) = \
		{ (rlock_pointer), nxu_rlock_irqsave(rlock_pointer) }

static inline uint64_t nxu_rlock_irqsave(nxu_rlock_t *rlock)
{
	uint64_t state = ml_irq_save();
	uint32_t me = machine_cpu_id() + 1U;

	if (__atomic_load_n(&rlock->owner, __ATOMIC_RELAXED) == me) {
		rlock->depth++;
		return state;
	}

	nxu_spin_lock(&rlock->lock);
	__atomic_store_n(&rlock->owner, me, __ATOMIC_RELAXED);
	rlock->depth = 1U;
	return state;
}

static inline void nxu_rlock_irqrestore(nxu_rlock_t *rlock, uint64_t state)
{
	if (--rlock->depth == 0U) {
		__atomic_store_n(&rlock->owner, 0U, __ATOMIC_RELAXED);
		nxu_spin_unlock(&rlock->lock);
	}

	ml_irq_restore(state);
}

#endif
