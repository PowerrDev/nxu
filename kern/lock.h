#ifndef NXU_KERN_LOCK_H
#define NXU_KERN_LOCK_H

#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>

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

#endif
