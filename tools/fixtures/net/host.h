/* Kernel-service substitutes for the single-threaded packet regression harness. */
#ifndef NXU_NET_TEST_HOST_H
#define NXU_NET_TEST_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
	uint32_t value;
} nxu_spinlock_t;

typedef void *thread_t;
typedef void *task_t;
typedef void *proc_t;

typedef struct {
	uint32_t value;
} nxu_cpuset_t;

static inline uint64_t
nxu_spin_lock_irqsave(nxu_spinlock_t *lock)
{
	(void)lock;
	return 0;
}

static inline void
nxu_spin_unlock_irqrestore(nxu_spinlock_t *lock, uint64_t irq)
{
	(void)lock;
	(void)irq;
}

static inline void
cpuset_clear(nxu_cpuset_t *set)
{
	set->value = 0;
}

static inline void
cpuset_add(nxu_cpuset_t *set, uint32_t cpu)
{
	set->value |= 1U << cpu;
}

static inline proc_t
proc_kernel(void)
{
	return (void *)1;
}

static inline task_t
proc_task(proc_t process)
{
	return process;
}

static inline bool
kernel_thread_create(task_t task, void (*run)(void *), void *context, thread_t *result)
{
	(void)task;
	(void)run;
	(void)context;
	*result = (void *)1;
	return true;
}

static inline bool
thread_set_affinity(thread_t thread, const nxu_cpuset_t *set)
{
	(void)thread;
	(void)set;
	return true;
}

static inline bool
sched_thread_start(thread_t thread)
{
	(void)thread;
	return true;
}

static inline bool
thread_terminate(thread_t thread)
{
	(void)thread;
	return true;
}

static inline bool
thread_reap(thread_t thread)
{
	(void)thread;
	return true;
}

static inline void
thread_deallocate(thread_t thread)
{
	(void)thread;
}

static inline void
kputln(const char *text)
{
	(void)text;
}

uint64_t timer_get_microseconds(void);
bool sched_sleep_us(uint64_t microseconds);
bool sched_yield(void);

#endif
