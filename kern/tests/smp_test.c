/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/smp_test.c
 *
 * See kern/tests/smp_test.h.
 *
 * Everything here runs as kernel threads and touches shared state only through
 * atomics, spinlocks, wait queues and the memory subsystems' own locking, so it
 * is safe to run on any CPU. Workers never print (the coordinator, on the boot
 * CPU, reports); a worker that finds something wrong counts it, and the
 * coordinator fails the test.
 */

#include <kern/tests/smp_test.h>

#include <kern/console/console.h>
#include <kern/cpuset.h>
#include <kern/lock.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>
#include <kern/machine/timer.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/processor.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/vm_fault.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SMP_TEST_MAX_THREADS 40U

static uint32_t g_ncpu;
static thread_t g_threads[SMP_TEST_MAX_THREADS];
static uint32_t g_thread_total;

/* ---- helpers ------------------------------------------------------------- */

static uint64_t now_us(void)
{
	return timer_get_microseconds();
}

static void cpus_only(nxu_cpuset_t *set, uint32_t cpu)
{
	cpuset_clear(set);
	cpuset_add(set, cpu);
}

/* Every online CPU but the boot CPU (which also runs the legacy threads and the coordinator). */
static void cpus_not_boot(nxu_cpuset_t *set)
{
	cpuset_clear(set);
	for (uint32_t cpu = 1U; cpu < g_ncpu; cpu++) cpuset_add(set, cpu);
}

static void cpus_all(nxu_cpuset_t *set)
{
	cpuset_clear(set);
	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) cpuset_add(set, cpu);
}

static bool spawn(thread_continue_t function, void *argument, const nxu_cpuset_t *affinity, bool preemptible)
{
	thread_t thread;

	if (g_thread_total >= SMP_TEST_MAX_THREADS) return false;
	if (!kernel_thread_create(proc_task(proc_kernel()), function, argument, &thread)) return false;

	if (affinity != 0 && !thread_set_affinity(thread, affinity)) return false;
	if (preemptible && !thread_set_preemptible(thread, true)) return false;
	if (!sched_thread_start(thread)) return false;

	g_threads[g_thread_total++] = thread;
	return true;
}

static void release_threads(void)
{
	for (uint32_t index = 0U; index < g_thread_total; index++) thread_deallocate(g_threads[index]);

	g_thread_total = 0U;
}

/* The coordinator waits by yielding, so workers pinned to the boot CPU (and the legacy threads) still run. */
static bool wait_for(volatile uint32_t *counter, uint32_t target, uint64_t timeout_us)
{
	uint64_t deadline = now_us() + timeout_us;

	while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < target) {
		if (now_us() > deadline) return false;
		(void)sched_yield();
	}

	return true;
}

static void pause_us(uint64_t microseconds)
{
	uint64_t deadline = now_us() + microseconds;

	while (now_us() < deadline) (void)sched_yield();
}

static bool check(bool condition, const char *name, const char *what)
{
	if (!condition) kprintf("smp_test: %s: FAILED: %s\n", name, what);
	return condition;
}

/* ---- 1: CPUs, per-CPU state, per-CPU timers ---------------------------- */

static bool test_cpus_online(void)
{
	const char *name = "cpus";

	if (!check(g_ncpu >= 2U, name, "fewer than two CPUs online")) return false;

	void *stacks[NXU_MAX_CPUS] = { 0 };

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) {
		processor_t processor = processor_by_id(cpu);

		if (!check(processor != 0 && processor_is_online(processor), name, "a CPU is not online")) return false;
		if (!check(processor->cpu_id == cpu, name, "processor id does not match its slot")) return false;
		if (!check(processor->idle_thread != 0 && thread_is_idle(processor->idle_thread), name, "a CPU has no idle thread")) return false;
		if (!check(cpu == 0U || processor->boot_stack != 0, name, "a secondary CPU has no boot stack")) return false;
		if (!check(processor_validate(processor), name, "processor_validate failed")) return false;

		for (uint32_t other = 0U; other < cpu; other++) {
			if (!check(cpu == 0U || stacks[other] != processor->boot_stack, name, "two CPUs share a boot stack")) return false;
		}

		stacks[cpu] = processor->boot_stack;
	}

	/* Every CPU takes its own timer interrupts: 5 ticks each within a second. */
	uint64_t before[NXU_MAX_CPUS];

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) before[cpu] = processor_by_id(cpu)->ticks;

	uint64_t deadline = now_us() + 1000000ULL;
	bool ticking = false;

	while (!ticking && now_us() < deadline) {
		ticking = true;

		for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) {
			if (processor_by_id(cpu)->ticks < before[cpu] + 5ULL) ticking = false;
		}

		(void)sched_yield();
	}

	return check(ticking, name, "a CPU's timer is not ticking");
}

/* ---- 2: one pinned thread per CPU, running at the same time ------------- */

static struct {
	volatile uint32_t arrived;
	volatile uint32_t done;
	volatile uint32_t wrong_cpu;
	volatile uint32_t barrier_timeouts;
} g_pin;

static void pin_worker(void *argument)
{
	uint32_t cpu = (uint32_t)(uintptr_t)argument;

	__atomic_add_fetch(&g_pin.arrived, 1U, __ATOMIC_ACQ_REL);

	/*
	 * A barrier without yielding: it can only be passed when every CPU is
	 * executing its worker at once. One CPU running them one after another
	 * would time out.
	 */
	uint64_t deadline = now_us() + 3000000ULL;

	while (__atomic_load_n(&g_pin.arrived, __ATOMIC_ACQUIRE) < g_ncpu) {
		if (now_us() > deadline) {
			__atomic_add_fetch(&g_pin.barrier_timeouts, 1U, __ATOMIC_ACQ_REL);
			break;
		}

		cpu_relax();
	}

	for (uint32_t round = 0U; round < 200U; round++) {
		if (machine_cpu_id() != cpu || current_processor()->cpu_id != cpu) {
			__atomic_add_fetch(&g_pin.wrong_cpu, 1U, __ATOMIC_ACQ_REL);
		}

		(void)sched_yield();
	}

	__atomic_add_fetch(&g_pin.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_pinned_per_cpu(void)
{
	const char *name = "pinned";

	memset(&g_pin, 0, sizeof(g_pin));

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) {
		nxu_cpuset_t only;

		cpus_only(&only, cpu);
		if (!check(spawn(pin_worker, (void *)(uintptr_t)cpu, &only, false), name, "could not start a pinned worker")) return false;
	}

	bool finished = wait_for(&g_pin.done, g_ncpu, 10000000ULL);

	release_threads();

	if (!check(finished, name, "pinned workers did not finish")) return false;
	if (!check(g_pin.barrier_timeouts == 0U, name, "the workers were not all running at the same time")) return false;
	return check(g_pin.wrong_cpu == 0U, name, "a pinned worker ran on the wrong CPU");
}

/* ---- 3: MLFQ on every CPU: more threads than CPUs, all preemptive ------- */

#define MLFQ_THREADS_PER_CPU 3U

static struct {
	volatile uint32_t stop;
	volatile uint32_t started;
	volatile uint32_t done;
	volatile uint32_t violations;
	volatile uint64_t progress[SMP_TEST_MAX_THREADS];
	volatile uint32_t in_body[SMP_TEST_MAX_THREADS];
	volatile uint64_t cpu_hits[NXU_MAX_CPUS];
} g_mlfq;

static void mlfq_worker(void *argument)
{
	uint32_t index = (uint32_t)(uintptr_t)argument;

	__atomic_add_fetch(&g_mlfq.started, 1U, __ATOMIC_ACQ_REL);

	while (!__atomic_load_n(&g_mlfq.stop, __ATOMIC_ACQUIRE)) {
		/* One instance of this thread at a time: a thread on two CPUs would find its own flag set. */
		if (__atomic_exchange_n(&g_mlfq.in_body[index], 1U, __ATOMIC_ACQ_REL) != 0U) {
			__atomic_add_fetch(&g_mlfq.violations, 1U, __ATOMIC_ACQ_REL);
		}

		/* With interrupts masked the CPU cannot change: the scheduler's own view must agree with ours. */
		uint64_t irq_state = ml_irq_save();
		thread_t self = current_thread();
		processor_t processor = current_processor();

		if (self == 0 || self->runq != 0 || self->on_cpu != processor || processor->active_thread != self) {
			__atomic_add_fetch(&g_mlfq.violations, 1U, __ATOMIC_ACQ_REL);
		}

		__atomic_add_fetch(&g_mlfq.cpu_hits[processor->cpu_id], 1ULL, __ATOMIC_RELAXED);
		ml_irq_restore(irq_state);

		g_mlfq.progress[index]++;

		__atomic_store_n(&g_mlfq.in_body[index], 0U, __ATOMIC_RELEASE);
	}

	__atomic_add_fetch(&g_mlfq.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_mlfq_concurrent(void)
{
	const char *name = "mlfq";
	uint32_t count = g_ncpu * MLFQ_THREADS_PER_CPU;
	nxu_cpuset_t all;

	memset((void *)&g_mlfq, 0, sizeof(g_mlfq));
	cpus_all(&all);

	for (uint32_t index = 0U; index < count; index++) {
		if (!check(spawn(mlfq_worker, (void *)(uintptr_t)index, &all, true), name, "could not start a worker")) {
			__atomic_store_n(&g_mlfq.stop, 1U, __ATOMIC_RELEASE);
			return false;
		}
	}

	bool valid = true;

	/* Run for a while, checking the run queues' invariants as the workers churn through them. */
	for (uint32_t sample = 0U; sample < 10U; sample++) {
		pause_us(60000ULL);
		if (!sched_validate_all()) valid = false;
	}

	__atomic_store_n(&g_mlfq.stop, 1U, __ATOMIC_RELEASE);

	bool finished = wait_for(&g_mlfq.done, count, 10000000ULL);
	bool demoted = false;

	for (uint32_t index = 0U; index < count; index++) {
		if (g_threads[index]->mlfq_level > SCHED_MLFQ_TOP_LEVEL) demoted = true;
	}

	release_threads();

	if (!check(finished, name, "workers did not stop")) return false;
	if (!check(valid, name, "sched_validate_all failed while the workers ran")) return false;
	if (!check(g_mlfq.violations == 0U, name, "a thread was seen on two CPUs, or queued while running")) return false;

	for (uint32_t index = 0U; index < count; index++) {
		if (!check(g_mlfq.progress[index] != 0ULL, name, "a thread never ran (starved)")) return false;
	}

	/*
	 * Every CPU that is free to take the work ran some of it. The boot CPU is
	 * left out of the requirement: it also runs the coordinator and every legacy
	 * thread (bootd, the boot chime), so it can be busier than the others, and
	 * placement then rightly sends the workers elsewhere.
	 */
	kprintf("smp_test: mlfq: iterations per CPU:");
	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) kprintf(" cpu%u=%llu", cpu, (unsigned long long)g_mlfq.cpu_hits[cpu]);
	kprintf("\n");

	for (uint32_t cpu = 1U; cpu < g_ncpu; cpu++) {
		if (!check(g_mlfq.cpu_hits[cpu] != 0ULL, name, "a secondary CPU never ran one of the workers")) return false;
	}

	return check(demoted, name, "no CPU-bound thread was demoted");
}

/* ---- 4: rapid sleep and wakeup across CPUs ------------------------------ */

#define PING_PONG_ROUNDS 1000U

static struct {
	nxu_spinlock_t guard;
	waitq_t queue[2];
	uint32_t turn;
	volatile uint32_t rounds[2];
	volatile uint32_t done;
} g_pp;

static void ping_pong_worker(void *argument)
{
	uint32_t me = (uint32_t)(uintptr_t)argument;
	uint32_t other = 1U - me;

	for (uint32_t round = 0U; round < PING_PONG_ROUNDS; round++) {
		uint64_t irq_state = nxu_spin_lock_irqsave(&g_pp.guard);

		while (g_pp.turn != me) {
			/*
			 * The condition was checked under the guard the other side needs to
			 * change it: the wait queue is joined before the guard is released,
			 * so its wakeup cannot fall between the check and the sleep.
			 */
			(void)waitq_block_unlock(&g_pp.queue[me], false, &g_pp.guard, irq_state);
			irq_state = nxu_spin_lock_irqsave(&g_pp.guard);
		}

		g_pp.turn = other;
		nxu_spin_unlock_irqrestore(&g_pp.guard, irq_state);

		waitq_wake_one(&g_pp.queue[other]);
		g_pp.rounds[me]++;
	}

	__atomic_add_fetch(&g_pp.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_ping_pong(void)
{
	const char *name = "wakeups";
	nxu_cpuset_t first;
	nxu_cpuset_t second;

	memset((void *)&g_pp, 0, sizeof(g_pp));
	waitq_init(&g_pp.queue[0]);
	waitq_init(&g_pp.queue[1]);

	cpus_only(&first, g_ncpu > 2U ? 1U : 0U);
	cpus_only(&second, g_ncpu > 2U ? 2U : 1U);

	if (!check(spawn(ping_pong_worker, (void *)0, &first, false), name, "could not start the first thread")) return false;
	if (!check(spawn(ping_pong_worker, (void *)1, &second, false), name, "could not start the second thread")) return false;

	bool finished = wait_for(&g_pp.done, 2U, 30000000ULL);

	release_threads();

	if (!check(finished, name, "the threads did not finish (a wakeup was lost)")) return false;
	return check(g_pp.rounds[0] == PING_PONG_ROUNDS && g_pp.rounds[1] == PING_PONG_ROUNDS, name, "wrong number of rounds");
}

/* ---- 5: reschedule IPIs ------------------------------------------------ */

static struct {
	volatile uint64_t started_at;
	volatile uint32_t done;
} g_ipi;

static void ipi_worker(void *argument)
{
	(void)argument;

	g_ipi.started_at = now_us();
	__atomic_store_n(&g_ipi.done, 1U, __ATOMIC_RELEASE);
}

static bool test_ipi(void)
{
	const char *name = "ipi";
	uint32_t target = g_ncpu - 1U;
	processor_t processor = processor_by_id(target);
	uint64_t ipis_before = processor->ipi_reschedule_count;
	uint64_t latency[20];
	nxu_cpuset_t only;

	cpus_only(&only, target);

	for (uint32_t trial = 0U; trial < 20U; trial++) {
		/* Let the target go idle and asleep in WFI first: that is the case an IPI exists for. */
		uint64_t deadline = now_us() + 100000ULL;

		while (__atomic_load_n(&processor->state, __ATOMIC_ACQUIRE) != PROCESSOR_IDLE && now_us() < deadline) (void)sched_yield();

		pause_us(15000ULL);

		thread_t thread;

		if (!check(kernel_thread_create(proc_task(proc_kernel()), ipi_worker, 0, &thread), name, "thread creation failed")) return false;
		if (!check(thread_set_affinity(thread, &only), name, "thread_set_affinity failed")) return false;

		__atomic_store_n(&g_ipi.done, 0U, __ATOMIC_RELEASE);

		uint64_t queued_at = now_us();

		if (!check(sched_thread_start(thread), name, "sched_thread_start failed")) return false;
		if (!check(wait_for(&g_ipi.done, 1U, 1000000ULL), name, "the worker never ran")) return false;

		latency[trial] = g_ipi.started_at - queued_at;
		thread_deallocate(thread);
	}

	/* Insertion sort, then the median: a lone slow trial (a device interrupt) is not the story. */
	for (uint32_t index = 1U; index < 20U; index++) {
		uint64_t value = latency[index];
		uint32_t place = index;

		while (place > 0U && latency[place - 1U] > value) {
			latency[place] = latency[place - 1U];
			place--;
		}

		latency[place] = value;
	}

	uint64_t ipis = processor->ipi_reschedule_count - ipis_before;

	kprintf("smp_test: ipi: wake-up latency of an idle CPU: median %llu us, worst %llu us, %llu IPIs taken\n", (unsigned long long)latency[10], (unsigned long long)latency[19], (unsigned long long)ipis);

	/* A tick is 10000 us: without the IPI a sleeping CPU would wait for it, so the median would be near 5000 us. */
	if (!check(ipis >= 10ULL, name, "the idle CPU took too few reschedule IPIs")) return false;
	return check(latency[10] < 3000ULL, name, "waking an idle CPU took as long as a timer tick");
}

/* ---- 6: affinity ------------------------------------------------------- */

static struct {
	volatile uint32_t seen_mask;
	volatile uint32_t done;
	volatile uint32_t stop;
	volatile uint32_t current_cpu;
} g_aff;

static void affinity_worker(void *argument)
{
	(void)argument;

	for (uint32_t round = 0U; round < 200U; round++) {
		__atomic_or_fetch(&g_aff.seen_mask, 1U << machine_cpu_id(), __ATOMIC_ACQ_REL);
		(void)sched_yield();
	}

	__atomic_add_fetch(&g_aff.done, 1U, __ATOMIC_ACQ_REL);
}

static void follower_worker(void *argument)
{
	(void)argument;

	while (!__atomic_load_n(&g_aff.stop, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&g_aff.current_cpu, machine_cpu_id(), __ATOMIC_RELEASE);
		(void)sched_yield();
	}

	__atomic_add_fetch(&g_aff.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_affinity(void)
{
	const char *name = "affinity";
	nxu_cpuset_t set;

	/* (a) a subset: threads allowed on every CPU but the boot CPU never run there. */
	memset((void *)&g_aff, 0, sizeof(g_aff));
	cpus_not_boot(&set);

	for (uint32_t index = 0U; index < 2U * g_ncpu; index++) {
		if (!check(spawn(affinity_worker, 0, &set, false), name, "could not start a subset worker")) return false;
	}

	bool finished = wait_for(&g_aff.done, 2U * g_ncpu, 20000000ULL);

	release_threads();

	if (!check(finished, name, "subset workers did not finish")) return false;
	if (!check((g_aff.seen_mask & 1U) == 0U, name, "a thread excluded from CPU 0 ran on it")) return false;

	uint32_t distinct = (uint32_t)__builtin_popcount(g_aff.seen_mask);

	if (!check(distinct >= (g_ncpu > 2U ? 2U : 1U), name, "the subset workers used too few CPUs")) return false;

	/* (b) pinned to the boot CPU: stays there. */
	memset((void *)&g_aff, 0, sizeof(g_aff));
	cpus_only(&set, 0U);

	if (!check(spawn(affinity_worker, 0, &set, false), name, "could not start the boot-CPU worker")) return false;

	finished = wait_for(&g_aff.done, 1U, 5000000ULL);
	release_threads();

	if (!check(finished && g_aff.seen_mask == 1U, name, "a thread pinned to the boot CPU ran elsewhere")) return false;

	/* (c) narrowing a running thread's affinity moves it: pinned to CPU 1, then to the last CPU. */
	memset((void *)&g_aff, 0, sizeof(g_aff));
	cpus_only(&set, 1U);

	if (!check(spawn(follower_worker, 0, &set, false), name, "could not start the follower")) return false;

	uint32_t moved_to = g_ncpu - 1U;
	bool on_first = wait_for(&g_aff.current_cpu, 1U, 2000000ULL) && g_aff.current_cpu == 1U;

	cpus_only(&set, moved_to);
	bool changed = thread_set_affinity(g_threads[0], &set);
	uint64_t deadline = now_us() + 2000000ULL;

	while (changed && __atomic_load_n(&g_aff.current_cpu, __ATOMIC_ACQUIRE) != moved_to && now_us() < deadline) (void)sched_yield();

	bool followed = __atomic_load_n(&g_aff.current_cpu, __ATOMIC_ACQUIRE) == moved_to;

	__atomic_store_n(&g_aff.stop, 1U, __ATOMIC_RELEASE);
	finished = wait_for(&g_aff.done, 1U, 2000000ULL);
	release_threads();

	if (!check(on_first, name, "the follower did not start on its first CPU")) return false;
	if (!check(changed && followed, name, "a thread did not follow its new affinity")) return false;
	return check(finished, name, "the follower did not stop");
}

/* ---- 7: an overloaded CPU is relieved by migration ---------------------- */

#define BALANCE_SPIN_US 60000ULL

static struct {
	volatile uint32_t done;
	volatile uint32_t seen_mask;
} g_bal;

static void balance_worker(void *argument)
{
	(void)argument;

	/* Cooperative, non-yielding: a thread queued behind one of these can only be helped by another CPU taking it. */
	uint64_t deadline = now_us() + BALANCE_SPIN_US;

	__atomic_or_fetch(&g_bal.seen_mask, 1U << machine_cpu_id(), __ATOMIC_ACQ_REL);

	while (now_us() < deadline) cpu_relax();

	__atomic_add_fetch(&g_bal.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_balance(void)
{
	const char *name = "balance";
	uint32_t count = 3U * g_ncpu;
	nxu_cpuset_t pinned;
	nxu_cpuset_t spread;

	memset((void *)&g_bal, 0, sizeof(g_bal));
	cpus_only(&pinned, 1U);
	cpus_not_boot(&spread);

	uint64_t moved_before = 0ULL;

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) moved_before += processor_by_id(cpu)->migrate_out_count;

	uint64_t started = now_us();

	/* Everything lands on CPU 1's queue... */
	for (uint32_t index = 0U; index < count; index++) {
		if (!check(spawn(balance_worker, 0, &pinned, false), name, "could not start a worker")) return false;
	}

	/* ...and then may run on any CPU but the boot CPU: the idle ones have to pull it away. */
	for (uint32_t index = 0U; index < count; index++) {
		if (!check(thread_set_affinity(g_threads[index], &spread), name, "thread_set_affinity failed")) return false;
	}

	bool finished = wait_for(&g_bal.done, count, 20000000ULL);
	uint64_t elapsed = now_us() - started;
	uint64_t moved = 0ULL;

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) moved += processor_by_id(cpu)->migrate_out_count;

	moved -= moved_before;
	release_threads();

	uint32_t distinct = (uint32_t)__builtin_popcount(g_bal.seen_mask);

	kprintf("smp_test: balance: %u threads, %llu us, %u CPUs used, %llu migrated by the balancer\n", count, (unsigned long long)elapsed, distinct, (unsigned long long)moved);

	if (!check(finished, name, "the workers did not finish")) return false;
	if (!check(distinct >= (g_ncpu > 2U ? 3U : 1U), name, "the load was not spread over the CPUs")) return false;
	if (!check(g_ncpu <= 2U || moved > 0ULL, name, "the balancer never migrated a thread")) return false;

	/* Serial on one CPU it is count * BALANCE_SPIN_US; with three CPUs helping it must beat that clearly. */
	return check(g_ncpu <= 3U || elapsed < (uint64_t)count * BALANCE_SPIN_US * 3ULL / 4ULL, name, "migration did not speed up the overloaded CPU");
}

/* ---- 8: contention, thread churn ---------------------------------------- */

#define LOCK_ITERATIONS 20000U

static struct {
	nxu_spinlock_t lock;
	uint64_t counter;
	volatile uint32_t done;
} g_lock;

static void lock_worker(void *argument)
{
	(void)argument;

	for (uint32_t iteration = 0U; iteration < LOCK_ITERATIONS; iteration++) {
		uint64_t irq_state = nxu_spin_lock_irqsave(&g_lock.lock);

		/* Deliberately not atomic: only the lock keeps the count exact. */
		g_lock.counter++;
		nxu_spin_unlock_irqrestore(&g_lock.lock, irq_state);

		if ((iteration & 1023U) == 0U) (void)sched_yield();
	}

	__atomic_add_fetch(&g_lock.done, 1U, __ATOMIC_ACQ_REL);
}

#define CHURN_CHILDREN 40U

static struct {
	nxu_cpuset_t affinity;
	volatile uint32_t spawners_done;
	volatile uint32_t children_done;
	volatile uint32_t failures;
} g_churn;

static void churn_child(void *argument)
{
	(void)argument;

	__atomic_add_fetch(&g_churn.children_done, 1U, __ATOMIC_ACQ_REL);
}

static void churn_spawner(void *argument)
{
	(void)argument;

	for (uint32_t child = 0U; child < CHURN_CHILDREN; child++) {
		thread_t thread;

		if (!kernel_thread_create(proc_task(proc_kernel()), churn_child, 0, &thread)) {
			__atomic_add_fetch(&g_churn.failures, 1U, __ATOMIC_ACQ_REL);
			continue;
		}

		if (!thread_set_affinity(thread, &g_churn.affinity) || !sched_thread_start(thread)) {
			__atomic_add_fetch(&g_churn.failures, 1U, __ATOMIC_ACQ_REL);
		}

		thread_deallocate(thread);

		/* Not waiting for this child in particular: the total is checked at the end. */
		(void)sched_yield();
	}

	__atomic_add_fetch(&g_churn.spawners_done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_stress(void)
{
	const char *name = "stress";
	nxu_cpuset_t all;

	cpus_all(&all);

	/* (a) lock contention: the total is exact only if the lock excludes across CPUs. */
	memset((void *)&g_lock, 0, sizeof(g_lock));

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) {
		if (!check(spawn(lock_worker, 0, &all, false), name, "could not start a lock worker")) return false;
	}

	bool finished = wait_for(&g_lock.done, g_ncpu, 30000000ULL);

	release_threads();

	if (!check(finished, name, "lock workers did not finish")) return false;
	if (!check(g_lock.counter == (uint64_t)g_ncpu * LOCK_ITERATIONS, name, "the contended counter is wrong: the lock does not exclude")) return false;

	/* (b) threads created and exiting on every CPU while the others do the same. */
	uint32_t baseline = thread_count();

	memset((void *)&g_churn, 0, sizeof(g_churn));
	g_churn.affinity = all;

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) {
		nxu_cpuset_t only;

		cpus_only(&only, cpu);
		if (!check(spawn(churn_spawner, 0, &only, false), name, "could not start a spawner")) return false;
	}

	finished = wait_for(&g_churn.spawners_done, g_ncpu, 30000000ULL);
	finished = finished && wait_for(&g_churn.children_done, g_ncpu * CHURN_CHILDREN, 30000000ULL);

	release_threads();

	if (!check(finished, name, "the spawners or their children did not finish")) return false;
	if (!check(g_churn.failures == 0U, name, "thread creation or start failed under load")) return false;

	/* Every child exited and was reaped: the pool is back to where it started. */
	uint64_t deadline = now_us() + 2000000ULL;

	while (thread_count() > baseline && now_us() < deadline) (void)sched_yield();

	return check(thread_count() <= baseline, name, "threads were not reaped after exiting on other CPUs");
}

/* ---- 9: memory shared by all CPUs, and TLB shootdown -------------------- */

#define MEMORY_ITERATIONS 120U

static struct {
	volatile uint32_t done;
	volatile uint32_t errors;
} g_mem;

static void memory_worker(void *argument)
{
	uint64_t tag = (uint64_t)(uintptr_t)argument * 0x0101010101010101ULL;

	for (uint32_t iteration = 0U; iteration < MEMORY_ITERATIONS; iteration++) {
		/*
		 * The page allocator directly, from every CPU at once: a page handed to
		 * two CPUs shows as one of them finding another's tag in it.
		 */
		uint64_t pages[16];
		uint32_t got = 0U;

		for (uint32_t index = 0U; index < 16U; index++) {
			uint64_t alias;

			if (!pmm_allocate_page(&pages[got]) || !vmm_physical_to_higher_half(pages[got], &alias)) continue;

			*(volatile uint64_t *)alias = tag + index;
			got++;
		}

		for (uint32_t index = 0U; index < got; index++) {
			uint64_t alias;

			if (vmm_physical_to_higher_half(pages[index], &alias) && *(volatile uint64_t *)alias != tag + index) {
				__atomic_add_fetch(&g_mem.errors, 1U, __ATOMIC_ACQ_REL);
			}

			(void)pmm_free_page(pages[index]);
		}

		size_t length = 200U + (iteration % 64U);
		uint8_t *block = kmalloc(length);

		if (block == 0) {
			__atomic_add_fetch(&g_mem.errors, 1U, __ATOMIC_ACQ_REL);
			continue;
		}

		memset(block, (int)(tag & 0xFFU) ^ (int)iteration, length);

		void *region;

		if (!vm_kern_allocate(3U * 4096U, VMM_PROTECTION_READ_WRITE, &region)) {
			__atomic_add_fetch(&g_mem.errors, 1U, __ATOMIC_ACQ_REL);
			(void)kfree(block);
			continue;
		}

		uint64_t *words = region;

		for (uint32_t index = 0U; index < (3U * 4096U) / 8U; index++) words[index] = tag + index + iteration;

		(void)sched_yield();

		for (uint32_t index = 0U; index < (3U * 4096U) / 8U; index++) {
			if (words[index] != tag + index + iteration) {
				__atomic_add_fetch(&g_mem.errors, 1U, __ATOMIC_ACQ_REL);
				break;
			}
		}

		for (size_t index = 0U; index < length; index++) {
			if (block[index] != (uint8_t)((tag & 0xFFU) ^ iteration)) {
				__atomic_add_fetch(&g_mem.errors, 1U, __ATOMIC_ACQ_REL);
				break;
			}
		}

		if (!vm_kern_free(region, 3U * 4096U)) __atomic_add_fetch(&g_mem.errors, 1U, __ATOMIC_ACQ_REL);
		if (!kfree(block)) __atomic_add_fetch(&g_mem.errors, 1U, __ATOMIC_ACQ_REL);
	}

	__atomic_add_fetch(&g_mem.done, 1U, __ATOMIC_ACQ_REL);
}

#define TLB_ROUNDS 300U
#define TLB_MAGIC 0x544C42000000ULL
#define TLB_POISON 0xDEADBEEFCAFEF00DULL

static struct {
	volatile uint64_t *volatile address;
	volatile uint64_t expected;
	volatile uint32_t published;
	volatile uint32_t acked;
	volatile uint32_t done;
	volatile uint32_t mismatches;
	volatile uint32_t same_address;
} g_tlb;

/*
 * The writer maps a page, has the reader touch it (so the reader's TLB holds
 * the translation), unmaps it, and takes the freed physical page for itself
 * before mapping a different one at the same virtual address. A reader whose TLB
 * entry survived the unmap would read the old page: the inner-shareable
 * invalidation is what makes it read the new one.
 */
static void tlb_writer(void *argument)
{
	(void)argument;

	volatile uint64_t *previous = 0;
	uint64_t spare = 0ULL;
	bool holding = false;

	for (uint32_t round = 1U; round <= TLB_ROUNDS; round++) {
		void *region;

		if (!vm_kern_allocate(4096U, VMM_PROTECTION_READ_WRITE, &region)) {
			__atomic_add_fetch(&g_tlb.mismatches, 1U, __ATOMIC_ACQ_REL);
			break;
		}

		/*
		 * Only now give back the page kept from the last round: while it was
		 * held the allocator could not hand it out again, so the page mapped
		 * here is a different one from the one the reader has cached.
		 */
		if (holding) (void)pmm_free_page(spare);

		holding = false;

		volatile uint64_t *word = region;

		if (word == previous) g_tlb.same_address++;
		previous = word;

		*word = TLB_MAGIC + round;
		g_tlb.expected = TLB_MAGIC + round;
		g_tlb.address = word;
		__atomic_store_n(&g_tlb.published, round, __ATOMIC_RELEASE);

		while (__atomic_load_n(&g_tlb.acked, __ATOMIC_ACQUIRE) != round) cpu_relax();

		(void)vm_kern_free(region, 4096U);

		/* Take the page that was just freed (the lowest free one), and fill it with something else. */
		if (pmm_allocate_page(&spare)) {
			uint64_t alias;

			holding = true;
			if (vmm_physical_to_higher_half(spare, &alias)) *(volatile uint64_t *)alias = TLB_POISON;
		}
	}

	if (holding) (void)pmm_free_page(spare);

	__atomic_add_fetch(&g_tlb.done, 1U, __ATOMIC_ACQ_REL);
}

static void tlb_reader(void *argument)
{
	(void)argument;

	for (uint32_t round = 1U; round <= TLB_ROUNDS; round++) {
		while (__atomic_load_n(&g_tlb.published, __ATOMIC_ACQUIRE) != round) cpu_relax();

		if (*g_tlb.address != g_tlb.expected) __atomic_add_fetch(&g_tlb.mismatches, 1U, __ATOMIC_ACQ_REL);

		__atomic_store_n(&g_tlb.acked, round, __ATOMIC_RELEASE);
	}

	__atomic_add_fetch(&g_tlb.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_memory(void)
{
	const char *name = "memory";
	nxu_cpuset_t all;
	uint64_t free_before = pmm_get_free_page_count();

	cpus_all(&all);
	memset((void *)&g_mem, 0, sizeof(g_mem));

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) {
		if (!check(spawn(memory_worker, (void *)(uintptr_t)(cpu + 1U), &all, false), name, "could not start a memory worker")) return false;
	}

	bool finished = wait_for(&g_mem.done, g_ncpu, 60000000ULL);

	release_threads();

	if (!check(finished, name, "memory workers did not finish")) return false;
	if (!check(g_mem.errors == 0U, name, "the heap, kernel VM or page allocator gave a wrong answer under concurrency")) return false;

	/* (b) TLB shootdown across CPUs. */
	nxu_cpuset_t writer_cpu;
	nxu_cpuset_t reader_cpu;

	memset((void *)&g_tlb, 0, sizeof(g_tlb));
	cpus_only(&writer_cpu, g_ncpu > 2U ? 1U : 0U);
	cpus_only(&reader_cpu, g_ncpu > 2U ? 2U : 1U);

	if (!check(spawn(tlb_reader, 0, &reader_cpu, false), name, "could not start the TLB reader")) return false;
	if (!check(spawn(tlb_writer, 0, &writer_cpu, false), name, "could not start the TLB writer")) return false;

	finished = wait_for(&g_tlb.done, 2U, 60000000ULL);

	release_threads();

	kprintf("smp_test: memory: %u of %u rounds reused the same kernel virtual address\n", g_tlb.same_address, TLB_ROUNDS);

	if (!check(finished, name, "the TLB writer and reader did not finish")) return false;
	if (!check(g_tlb.mismatches == 0U, name, "a CPU read through a stale translation after the page was unmapped")) return false;

	/* Nothing leaked: the pages the workers took were all given back. */
	return check(pmm_get_free_page_count() + 8ULL >= free_before, name, "pages were leaked");
}

/* ---- 10: user address spaces shared by several CPUs ---------------------- */

/*
 * These drive the user-space VM code (vm/address_space.c, vm_fault.c, vm_map.c)
 * directly from kernel threads on different CPUs, on synthetic address spaces
 * that no process owns, so that the races can be aimed precisely: a page
 * unmapped while another CPU holds its translation, a page faulted in while the
 * region is being unmapped, and the same copy-on-write page broken by two CPUs
 * at once.
 */

static bool user_space_new(vm_address_space_t *space)
{
	memset(space, 0, sizeof(*space));
	return vm_address_space_create(space);
}

static void user_space_free(vm_address_space_t *space)
{
	vm_map_destroy_all(space);
	(void)vm_address_space_release_pages(space);
	(void)vm_address_space_destroy(space);
}

static void user_cpus(nxu_cpuset_t *first, nxu_cpuset_t *second)
{
	cpus_only(first, g_ncpu > 2U ? 1U : 0U);
	cpus_only(second, g_ncpu > 2U ? 2U : 1U);
}

/* -- user TLB shootdown: unmap under a reader's cached translation -- */

#define UTLB_ROUNDS 300U
#define UTLB_VA 0x10000000ULL
#define UTLB_TAG 0x5554000000ULL

static struct {
	vm_address_space_t space;
	volatile uint32_t published;
	volatile uint32_t acked;
	volatile uint32_t done;
	volatile uint32_t mismatches;
	volatile uint64_t expected;
} g_utlb;

static void utlb_writer(void *argument)
{
	(void)argument;

	uint64_t held = 0ULL;
	bool holding = false;

	for (uint32_t round = 1U; round <= UTLB_ROUNDS; round++) {
		uint64_t physical;
		uint64_t alias;

		/* A different physical page every round: the previous one is still held. */
		if (!pmm_allocate_page(&physical) || !vmm_physical_to_higher_half(physical, &alias)) {
			__atomic_add_fetch(&g_utlb.mismatches, 1U, __ATOMIC_ACQ_REL);
			break;
		}

		*(volatile uint64_t *)alias = UTLB_TAG + round;

		if (holding) (void)pmm_free_page(held);

		holding = false;

		if (!vm_address_space_map_page(&g_utlb.space, UTLB_VA, physical, VM_USER_PROTECTION_READ_WRITE)) {
			__atomic_add_fetch(&g_utlb.mismatches, 1U, __ATOMIC_ACQ_REL);
			(void)pmm_free_page(physical);
			break;
		}

		g_utlb.expected = UTLB_TAG + round;
		__atomic_store_n(&g_utlb.published, round, __ATOMIC_RELEASE);

		while (__atomic_load_n(&g_utlb.acked, __ATOMIC_ACQUIRE) != round) cpu_relax();

		if (!vm_address_space_unmap_page(&g_utlb.space, UTLB_VA)) __atomic_add_fetch(&g_utlb.mismatches, 1U, __ATOMIC_ACQ_REL);

		/* What a stale translation would still reach now holds something else. */
		*(volatile uint64_t *)alias = TLB_POISON;
		held = physical;
		holding = true;
	}

	if (holding) (void)pmm_free_page(held);

	__atomic_add_fetch(&g_utlb.done, 1U, __ATOMIC_ACQ_REL);
}

static void utlb_reader(void *argument)
{
	(void)argument;

	for (uint32_t round = 1U; round <= UTLB_ROUNDS; round++) {
		while (__atomic_load_n(&g_utlb.published, __ATOMIC_ACQUIRE) != round) cpu_relax();

		/* Loaded into this CPU's TTBR0 once and left there: its TLB keeps the translation between rounds. */
		if (!vm_address_space_activate(&g_utlb.space)) {
			__atomic_add_fetch(&g_utlb.mismatches, 1U, __ATOMIC_ACQ_REL);
		} else if (*(volatile uint64_t *)UTLB_VA != g_utlb.expected) {
			__atomic_add_fetch(&g_utlb.mismatches, 1U, __ATOMIC_ACQ_REL);
		}

		__atomic_store_n(&g_utlb.acked, round, __ATOMIC_RELEASE);
	}

	(void)vm_address_space_deactivate();
	__atomic_add_fetch(&g_utlb.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_user_tlb(void)
{
	const char *name = "usertlb";
	nxu_cpuset_t writer_cpu;
	nxu_cpuset_t reader_cpu;

	memset((void *)&g_utlb, 0, sizeof(g_utlb));
	user_cpus(&writer_cpu, &reader_cpu);

	if (!check(user_space_new(&g_utlb.space), name, "could not create an address space")) return false;

	if (!check(spawn(utlb_reader, 0, &reader_cpu, false), name, "could not start the reader")) return false;
	if (!check(spawn(utlb_writer, 0, &writer_cpu, false), name, "could not start the writer")) return false;

	bool finished = wait_for(&g_utlb.done, 2U, 60000000ULL);

	release_threads();

	if (!check(finished, name, "the writer and reader did not finish")) return false;

	user_space_free(&g_utlb.space);

	return check(g_utlb.mismatches == 0U, name, "a CPU translated through a stale entry after a user page was unmapped");
}

/* -- a fault racing an munmap of its region -- */

#define VRACE_ITERATIONS 400U
#define VRACE_PAGES 8U
#define VRACE_BASE 0x20000000ULL

static struct {
	vm_address_space_t space;
	volatile uint32_t stop;
	volatile uint32_t done;
} g_vrace;

static void vrace_faulter(void *argument)
{
	uint32_t state = (uint32_t)(uintptr_t)argument * 2654435761U + 1U;

	while (!__atomic_load_n(&g_vrace.stop, __ATOMIC_ACQUIRE)) {
		state = state * 1664525U + 1013904223U;
		(void)vm_fault_user(&g_vrace.space, VRACE_BASE + (uint64_t)((state >> 16U) % VRACE_PAGES) * 4096ULL, VM_FAULT_WRITE);
	}

	__atomic_add_fetch(&g_vrace.done, 1U, __ATOMIC_ACQ_REL);
}

static bool test_fault_vs_unmap(void)
{
	const char *name = "vmrace";
	uint64_t free_before = pmm_get_free_page_count();
	nxu_cpuset_t first;
	nxu_cpuset_t second;

	memset((void *)&g_vrace, 0, sizeof(g_vrace));
	user_cpus(&first, &second);

	if (!check(user_space_new(&g_vrace.space), name, "could not create an address space")) return false;

	if (!check(spawn(vrace_faulter, (void *)1, &first, false), name, "could not start a faulter")) return false;
	if (!check(spawn(vrace_faulter, (void *)2, &second, false), name, "could not start a faulter")) return false;

	uint32_t stragglers = 0U;

	for (uint32_t iteration = 0U; iteration < VRACE_ITERATIONS; iteration++) {
		if (!check(vm_map_reserve(&g_vrace.space, VRACE_BASE, VRACE_BASE + (uint64_t)VRACE_PAGES * 4096ULL, VM_USER_PROTECTION_READ_WRITE), name, "vm_map_reserve failed")) break;

		/* Let the faulters populate part of it... */
		uint64_t until = now_us() + 20ULL + (uint64_t)(iteration % 7U) * 15ULL;

		while (now_us() < until) cpu_relax();

		/* ...then unmap it under them. */
		if (!check(vm_map_free(&g_vrace.space, VRACE_BASE, (uint64_t)VRACE_PAGES * 4096ULL), name, "vm_map_free failed")) break;

		/* From here no fault may put a page back into a region that no longer exists. */
		until = now_us() + 60ULL;

		while (now_us() < until) cpu_relax();

		for (uint32_t page = 0U; page < VRACE_PAGES; page++) {
			vm_user_page_mapping_t mapping;
			uint64_t address = VRACE_BASE + (uint64_t)page * 4096ULL;

			if (vm_address_space_query_page(&g_vrace.space, address, &mapping)) {
				stragglers++;
				(void)vm_address_space_unmap_page(&g_vrace.space, address);
				(void)pmm_free_page(mapping.physical_address);
			}
		}
	}

	__atomic_store_n(&g_vrace.stop, 1U, __ATOMIC_RELEASE);
	bool finished = wait_for(&g_vrace.done, 2U, 30000000ULL);

	release_threads();
	user_space_free(&g_vrace.space);

	if (!check(finished, name, "the faulters did not stop")) return false;
	if (!check(stragglers == 0U, name, "a page was mapped into a region after it had been unmapped")) return false;

	return check(pmm_get_free_page_count() + 8ULL >= free_before, name, "pages were leaked");
}

/* -- two CPUs breaking the same copy-on-write page -- */

#define COW_ROUNDS 150U
#define COW_VA 0x30000000ULL

static struct {
	vm_address_space_t parent;
	vm_address_space_t child;
	volatile uint32_t round;
	volatile uint32_t arrived[COW_ROUNDS + 1U];
	volatile uint32_t finished[COW_ROUNDS + 1U];
	volatile uint32_t failures;
	volatile uint32_t quit;
} g_cow;

static void cow_worker(void *argument)
{
	(void)argument;

	for (uint32_t round = 1U; round <= COW_ROUNDS; round++) {
		while (__atomic_load_n(&g_cow.round, __ATOMIC_ACQUIRE) < round) {
			if (__atomic_load_n(&g_cow.quit, __ATOMIC_ACQUIRE)) return;
			cpu_relax();
		}

		/* Both workers leave the barrier together and fault on the same shared page. */
		(void)__atomic_add_fetch(&g_cow.arrived[round], 1U, __ATOMIC_ACQ_REL);

		while (__atomic_load_n(&g_cow.arrived[round], __ATOMIC_ACQUIRE) < 2U) cpu_relax();

		if (!vm_fault_user(&g_cow.parent, COW_VA, VM_FAULT_WRITE)) __atomic_add_fetch(&g_cow.failures, 1U, __ATOMIC_ACQ_REL);

		(void)__atomic_add_fetch(&g_cow.finished[round], 1U, __ATOMIC_ACQ_REL);
	}
}

static bool test_cow_race(void)
{
	const char *name = "cowrace";
	uint64_t free_before = pmm_get_free_page_count();
	nxu_cpuset_t first;
	nxu_cpuset_t second;

	memset((void *)&g_cow, 0, sizeof(g_cow));
	user_cpus(&first, &second);

	if (!check(spawn(cow_worker, 0, &first, false), name, "could not start a worker")) return false;
	if (!check(spawn(cow_worker, 0, &second, false), name, "could not start a worker")) return false;

	bool ok = true;

	for (uint32_t round = 1U; round <= COW_ROUNDS && ok; round++) {
		uint64_t physical;
		uint64_t alias;

		ok = user_space_new(&g_cow.parent) && pmm_allocate_page(&physical) && vmm_physical_to_higher_half(physical, &alias);

		if (!ok) {
			kprintf("smp_test: %s: FAILED: setup\n", name);
			break;
		}

		*(volatile uint64_t *)alias = 0xC0DE000000ULL + round;

		ok = vm_map_reserve(&g_cow.parent, COW_VA, COW_VA + 4096ULL, VM_USER_PROTECTION_READ_WRITE) &&
			vm_address_space_map_page(&g_cow.parent, COW_VA, physical, VM_USER_PROTECTION_READ_WRITE) &&
			vm_address_space_fork(&g_cow.parent, &g_cow.child);

		if (!ok) {
			kprintf("smp_test: %s: FAILED: fork of the address space\n", name);
			break;
		}

		/* Both spaces now share the page copy-on-write; two CPUs break it in the parent at once. */
		__atomic_store_n(&g_cow.round, round, __ATOMIC_RELEASE);

		ok = wait_for(&g_cow.finished[round], 2U, 10000000ULL);

		vm_user_page_mapping_t parent_page;
		vm_user_page_mapping_t child_page;

		if (ok) {
			ok = vm_address_space_query_page(&g_cow.parent, COW_VA, &parent_page) &&
				vm_address_space_query_page(&g_cow.child, COW_VA, &child_page) &&
				parent_page.protection == VM_USER_PROTECTION_READ_WRITE && !parent_page.cow &&
				parent_page.physical_address != child_page.physical_address;

			uint64_t parent_alias;

			ok = ok && vmm_physical_to_higher_half(parent_page.physical_address, &parent_alias) &&
				*(volatile uint64_t *)parent_alias == 0xC0DE000000ULL + round;
		}

		if (!ok) kprintf("smp_test: %s: FAILED: after round %u the copy is wrong or was lost\n", name, round);

		/* Each mapping owns its page: the child's is the original, the parent's is the copy. */
		user_space_free(&g_cow.child);
		user_space_free(&g_cow.parent);
	}

	__atomic_store_n(&g_cow.quit, 1U, __ATOMIC_RELEASE);
	release_threads();

	if (!ok) return false;
	if (!check(g_cow.failures == 0U, name, "a COW fault that lost the race to another CPU was reported as a failure")) return false;

	return check(pmm_get_free_page_count() + 8ULL >= free_before, name, "pages were leaked");
}

/* ---- the suite ---------------------------------------------------------- */

typedef struct {
	const char *name;
	bool (*run)(void);
} smp_test_case_t;

bool smp_test_run(void)
{
	static const smp_test_case_t cases[] = {
		{ "cpus", test_cpus_online },
		{ "pinned", test_pinned_per_cpu },
		{ "mlfq", test_mlfq_concurrent },
		{ "wakeups", test_ping_pong },
		{ "ipi", test_ipi },
		{ "affinity", test_affinity },
		{ "balance", test_balance },
		{ "stress", test_stress },
		{ "memory", test_memory },
		{ "usertlb", test_user_tlb },
		{ "vmrace", test_fault_vs_unmap },
		{ "cowrace", test_cow_race }
	};

	g_ncpu = smp_online_count();

	kprintf("smp_test: %u CPUs online\n", g_ncpu);

	for (uint32_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
		kprintf("smp_test: %s: running\n", cases[index].name);

		if (!cases[index].run()) return false;
		if (!sched_validate_all()) {
			kprintf("smp_test: %s: FAILED: the run queues are inconsistent afterwards\n", cases[index].name);
			return false;
		}

		kprintf("smp_test: %s: ok\n", cases[index].name);
	}

	for (uint32_t cpu = 0U; cpu < g_ncpu; cpu++) {
		processor_t processor = processor_by_id(cpu);

		kprintf(
			"smp_test: cpu%u: %llu ticks, %llu switches, %llu preemptions, %llu IPIs, %llu migrated in, %llu out\n",
			cpu,
			(unsigned long long)processor->ticks,
			(unsigned long long)processor->context_switch_count,
			(unsigned long long)processor->preemption_count,
			(unsigned long long)processor->ipi_reschedule_count,
			(unsigned long long)processor->migrate_in_count,
			(unsigned long long)processor->migrate_out_count
		);
	}

	return true;
}
