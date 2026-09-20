#ifndef NXU_KERN_LOCK_H
#define NXU_KERN_LOCK_H

#include <kern/machine/cpu.h>

#include <stdint.h>

/*
 * Canonical form of the atomic-exchange spinlock every subsystem in this
 * kernel (thread_lock, sched_lock, ipc_space's lock, ...) already hand-rolls
 * individually. New locks should use this type instead of copying the
 * pattern again; existing ones may migrate to it opportunistically.
 */
typedef struct {
	volatile uint32_t value;
} nxu_spinlock_t;

static inline void nxu_spin_lock(nxu_spinlock_t *lock)
{
	while (
		__atomic_exchange_n(
			&lock->value,
			1U,
			__ATOMIC_ACQUIRE
		) != 0U
	) {
		cpu_relax();
	}
}

static inline void nxu_spin_unlock(nxu_spinlock_t *lock)
{
	__atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

#endif
