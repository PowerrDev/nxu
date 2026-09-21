#include <kern/machine/smp.h>
#include <kern/sched_prism/processor.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* machine_cpu_id() reads the first word of the per-CPU object as the logical id. */
_Static_assert(offsetof(struct processor, cpu_id) == 0, "struct processor must start with cpu_id");

static struct processor g_processors[NXU_MAX_CPUS];
static uint32_t g_processor_count;
static bool g_processor_initialized;

/*
 * Initialise one slot. The state stays OFFLINE until the CPU reports in
 * (boot CPU: immediately).
 */
static void processor_init_slot(processor_t processor, uint32_t cpu_id, uint64_t mpidr)
{
	memset(processor, 0, sizeof(*processor));
	processor->cpu_id = cpu_id;
	processor->mpidr = mpidr;
	processor->state = PROCESSOR_OFFLINE;
	run_queue_init(&processor->runq);
}

/*
 * processor_bootstrap
 */
bool processor_bootstrap(void)
{
	if (g_processor_initialized) return true;

	processor_init_slot(&g_processors[0], 0U, machine_cpu_mpidr());
	g_processors[0].state = PROCESSOR_STARTING;
	g_processors[0].state = PROCESSOR_RUNNING;

	g_processor_count = 1U;
	g_processor_initialized = true;

	/* From here `current_processor()` on the boot CPU is a register read. */
	machine_cpu_local_set(&g_processors[0]);
	return true;
}

/*
 * current_processor
 */
processor_t current_processor(void)
{
	void *local = machine_cpu_local();

	if (local != 0) return local;
	return g_processor_initialized ? &g_processors[0] : 0;
}

/*
 * processor_register
 */
processor_t processor_register(uint32_t cpu_id, uint64_t mpidr)
{
	if (!g_processor_initialized || cpu_id == 0U || cpu_id >= NXU_MAX_CPUS) return 0;
	if (cpu_id < g_processor_count && g_processors[cpu_id].mpidr != 0ULL) return 0;

	processor_init_slot(&g_processors[cpu_id], cpu_id, mpidr);

	if (cpu_id >= g_processor_count) g_processor_count = cpu_id + 1U;
	return &g_processors[cpu_id];
}

/*
 * processor_by_id
 */
processor_t processor_by_id(uint32_t cpu_id)
{
	if (!g_processor_initialized || cpu_id >= g_processor_count) return 0;
	return &g_processors[cpu_id];
}

/*
 * processor_count
 */
uint32_t processor_count(void)
{
	return g_processor_count;
}

/*
 * processor_online_set
 */
void processor_online_set(nxu_cpuset_t *set)
{
	cpuset_clear(set);

	for (uint32_t cpu = 0U; cpu < g_processor_count; cpu++) {
		processor_state_t state = __atomic_load_n(&g_processors[cpu].state, __ATOMIC_ACQUIRE);

		if (state == PROCESSOR_RUNNING || state == PROCESSOR_IDLE) cpuset_add(set, cpu);
	}
}

/*
 * The affinity new threads get. Written at boot or by a test before the threads
 * it concerns exist, read by every thread creation: plain reads of a word-sized
 * set are enough.
 */
static nxu_cpuset_t g_default_affinity = { { 1ULL } };

void processor_default_affinity(nxu_cpuset_t *affinity)
{
	*affinity = g_default_affinity;
}

void processor_set_default_affinity(const nxu_cpuset_t *affinity)
{
	if (affinity != 0 && !cpuset_empty(affinity)) g_default_affinity = *affinity;
}

/*
 * The affinity threads of user processes start with. Kept apart from the kernel
 * threads' default: user code reaches the kernel only through system calls, and
 * the ones that touch single-CPU subsystems bind themselves to the boot CPU
 * (sched_bind_boot_cpu), so a user thread can safely be given every CPU while
 * kernel threads (which call those subsystems directly) keep the boot CPU.
 */
static nxu_cpuset_t g_default_user_affinity = { { 1ULL } };

void processor_default_user_affinity(nxu_cpuset_t *affinity)
{
	*affinity = g_default_user_affinity;
}

void processor_set_default_user_affinity(const nxu_cpuset_t *affinity)
{
	if (affinity != 0 && !cpuset_empty(affinity)) g_default_user_affinity = *affinity;
}

/*
 * processor_is_online
 */
bool processor_is_online(const struct processor *processor)
{
	if (processor == 0) return false;

	processor_state_t state = __atomic_load_n(&processor->state, __ATOMIC_ACQUIRE);

	return state == PROCESSOR_RUNNING || state == PROCESSOR_IDLE;
}

/*
 * processor_load
 */
uint32_t processor_load(const struct processor *processor)
{
	if (processor == 0) return 0U;

	thread_t active = __atomic_load_n(&processor->active_thread, __ATOMIC_RELAXED);
	uint32_t queued = __atomic_load_n(&processor->runq.count, __ATOMIC_RELAXED);

	return queued + ((active != 0 && !thread_is_idle(active)) ? 1U : 0U);
}

/*
 * processor_validate
 */
bool processor_validate(processor_t processor)
{
	if (
		processor == 0 ||
		processor->cpu_id >= NXU_MAX_CPUS ||
		processor->state == PROCESSOR_OFFLINE ||
		processor->state == PROCESSOR_SHUTDOWN ||
		!run_queue_validate(&processor->runq)
	) {
		return false;
	}

	if (
		processor->idle_thread != 0 &&
		(!thread_is_active(processor->idle_thread) ||
		!thread_is_idle(processor->idle_thread) ||
		!thread_is_started(processor->idle_thread) ||
		processor->idle_thread->runq != 0)
	) {
		return false;
	}

	if (
		processor->active_thread != 0 &&
		(!thread_is_active(processor->active_thread) ||
		processor->active_thread->runq != 0)
	) {
		return false;
	}

	return true;
}
