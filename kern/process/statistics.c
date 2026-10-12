#include <kern/process/statistics.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/processor.h>
#include <kern/machine/timer.h>
#include <kern/memory/heap.h>
#include <kern/syscall/syscall_defs.h>
#include <vm/pmm.h>
#include <drivers/virtio/virtqueue.h>
#include <string.h>

typedef struct {
	nxu_spinlock_t lock;
	uint64_t page_faults, interrupts;
} cpu_events_t;

static cpu_events_t g_cpu_events[NXU_MAX_CPUS];

void
statistics_note_interrupt(void)
{
	processor_t cpu = current_processor();
	/* Before processor bootstrap only the boot CPU can take interrupts. */
	uint32_t id = cpu != 0 ? cpu->cpu_id : 0U;
	if (id >= NXU_MAX_CPUS) return;
	cpu_events_t *events = &g_cpu_events[id];
	uint64_t irq = nxu_spin_lock_irqsave(&events->lock);
	events->interrupts++;
	nxu_spin_unlock_irqrestore(&events->lock, irq);
}

void
statistics_note_page_fault(bool user)
{
	processor_t cpu = current_processor();
	uint32_t id = cpu != 0 ? cpu->cpu_id : 0U;
	if (id < NXU_MAX_CPUS) {
		cpu_events_t *events = &g_cpu_events[id];
		uint64_t irq = nxu_spin_lock_irqsave(&events->lock);
		events->page_faults++;
		nxu_spin_unlock_irqrestore(&events->lock, irq);
	}
	if (!user) return;
	thread_t thread = current_thread();
	if (thread == 0) return;
	uint64_t irq = nxu_spin_lock_irqsave(&thread->accounting_lock);
	thread->page_faults++;
	nxu_spin_unlock_irqrestore(&thread->accounting_lock, irq);
	task_t task = thread->home_task != 0 ? thread->home_task : thread->task;
	if (task != 0) {
		irq = nxu_spin_lock_irqsave(&task->event_lock);
		task->page_faults++;
		nxu_spin_unlock_irqrestore(&task->event_lock, irq);
	}
}

_Static_assert(PROC_NAME_MAX == NXU_STATS_NAME_MAX, "process name ABI");
_Static_assert(PROC_STATE_UNUSED == NXU_STATS_PROCESS_UNUSED && PROC_STATE_EMBRYO == NXU_STATS_PROCESS_EMBRYO &&
				   PROC_STATE_RUNNABLE == NXU_STATS_PROCESS_RUNNABLE &&
				   PROC_STATE_RUNNING == NXU_STATS_PROCESS_RUNNING && PROC_STATE_STOPPED == NXU_STATS_PROCESS_STOPPED &&
				   PROC_STATE_ZOMBIE == NXU_STATS_PROCESS_ZOMBIE && PROC_STATE_DEAD == NXU_STATS_PROCESS_DEAD,
			   "process state ABI");
_Static_assert(PROC_FLAG_SYSTEM == NXU_STATS_PROCESS_SYSTEM && PROC_FLAG_EXITING == NXU_STATS_PROCESS_EXITING &&
				   PROC_FLAG_REAPED == NXU_STATS_PROCESS_REAPED && PROC_FLAG_EXECING == NXU_STATS_PROCESS_EXECING &&
				   PROC_FLAG_TRACED == NXU_STATS_PROCESS_TRACED,
			   "process flags ABI");

uint32_t
statistics_record_size(uint32_t kind)
{
	switch (kind) {
	case NXU_STATS_SYSTEM:
		return sizeof(nxu_stats_system_t);
	case NXU_STATS_CPU:
		return sizeof(nxu_stats_cpu_t);
	case NXU_STATS_PROCESS:
		return sizeof(nxu_stats_process_t);
	case NXU_STATS_THREAD:
		return sizeof(nxu_stats_thread_t);
	case NXU_STATS_PROCESS_MEMORY:
		return sizeof(nxu_stats_process_memory_t);
	case NXU_STATS_CPU_EVENTS:
		return sizeof(nxu_stats_cpu_events_t);
	case NXU_STATS_THREAD_EVENTS:
		return sizeof(nxu_stats_thread_events_t);
	case NXU_STATS_VIRTIO:
		return sizeof(nxu_stats_virtio_t);
	default:
		return 0U;
	}
}

uint32_t
statistics_capacity(uint32_t kind)
{
	switch (kind) {
	case NXU_STATS_SYSTEM:
		return 1U;
	case NXU_STATS_CPU:
		return NXU_MAX_CPUS;
	case NXU_STATS_PROCESS:
		return PROC_MAX;
	case NXU_STATS_THREAD:
		return THREAD_MAX;
	case NXU_STATS_THREAD_EVENTS:
		return THREAD_MAX;
	case NXU_STATS_PROCESS_MEMORY:
		return PROC_MAX;
	case NXU_STATS_CPU_EVENTS:
		return NXU_MAX_CPUS;
	case NXU_STATS_VIRTIO:
		return 1U;
	default:
		return 0U;
	}
}

void
statistics_cpu_snapshot(uint32_t cpu, nxu_stats_cpu_t *out)
{
	*out = (nxu_stats_cpu_t){.struct_size = sizeof(*out), .version = NXU_STATS_VERSION, .cpu_id = cpu};
	processor_t processor = processor_by_id(cpu);
	if (processor == 0) return;
	/* These accounting writers hold runq_lock. Mask local IRQs before taking
	 * a remote queue lock; release before any user copy or allocation. */
	uint64_t irq = nxu_spin_lock_irqsave(&processor->runq_lock);
	out->sampled_us = timer_get_microseconds();
	out->busy_us = processor->busy_us;
	if (processor->busy_since_us != 0ULL && out->sampled_us > processor->busy_since_us)
		out->busy_us += out->sampled_us - processor->busy_since_us;
	out->context_switches = processor->context_switch_count;
	out->dispatches = processor->dispatch_count;
	out->preemptions = processor->preemption_count;
	out->quantum_expirations = processor->quantum_expiration_count;
	out->run_queue_depth = processor->runq.count;
	out->online = processor_is_online(processor) ? 1U : 0U;
	nxu_spin_unlock_irqrestore(&processor->runq_lock, irq);
}

int64_t
statistics_snapshot(uint32_t kind, void *records, uint32_t capacity, uint32_t *total)
{
	if (total == 0 || statistics_record_size(kind) == 0U || (capacity != 0U && records == 0))
		return -NXU_SYS_E_INVALID_ARGUMENT;
	if (kind == NXU_STATS_SYSTEM) {
		*total = 1U;
		if (capacity == 0U) return 0;
		nxu_stats_system_t *out = records;
		*out = (nxu_stats_system_t){.struct_size = sizeof(*out),
									.version = NXU_STATS_VERSION,
									.sampled_us = timer_get_microseconds(),
									.page_size = PMM_PAGE_SIZE,
									.total_pages = pmm_get_page_count(),
									.free_pages = pmm_get_free_page_count(),
									.heap_pages = heap_get_page_count(),
									.cpu_count = processor_count(),
									.process_count = proc_count(),
									.thread_count = thread_count()};
		return 1;
	}
	if (kind == NXU_STATS_CPU) {
		*total = processor_count();
		uint32_t count = capacity < *total ? capacity : *total;
		for (uint32_t cpu = 0U; cpu < count; cpu++)
			statistics_cpu_snapshot(cpu, &((nxu_stats_cpu_t *)records)[cpu]);
		return count;
	}
	if (kind == NXU_STATS_PROCESS_MEMORY) {
		*total = proc_memory_snapshot(records, capacity);
		return capacity < *total ? capacity : *total;
	}
	if (kind == NXU_STATS_CPU_EVENTS) {
		*total = processor_count();
		uint32_t count = capacity < *total ? capacity : *total;
		nxu_stats_cpu_events_t *out = records;
		for (uint32_t i = 0U; i < count; i++) {
			uint64_t irq = nxu_spin_lock_irqsave(&g_cpu_events[i].lock);
			out[i] = (nxu_stats_cpu_events_t){.struct_size = sizeof(*out),
											  .version = NXU_STATS_VERSION,
											  .cpu_id = i,
											  .page_faults = g_cpu_events[i].page_faults,
											  .interrupts = g_cpu_events[i].interrupts};
			nxu_spin_unlock_irqrestore(&g_cpu_events[i].lock, irq);
		}
		return count;
	}
	if (kind == NXU_STATS_VIRTIO) {
		*total = 1U;
		if (capacity == 0U) return 0;
		virtqueue_statistics(records);
		return 1;
	}
	/* Caller-local storage makes simultaneous observers independent. Table
	 * locks are taken separately, so process/thread joins are best effort. */
	thread_snapshot_t *threads = kmalloc(THREAD_MAX * sizeof(*threads));
	if (threads == 0) return -NXU_SYS_E_NO_MEMORY;
	uint32_t thread_total = thread_snapshot(threads, THREAD_MAX);
	int64_t result;
	if (kind == NXU_STATS_THREAD_EVENTS) {
		*total = thread_total;
		uint32_t count = capacity < *total ? capacity : *total;
		nxu_stats_thread_events_t *out = records;
		for (uint32_t i = 0U; i < count; i++)
			out[i] = (nxu_stats_thread_events_t){.struct_size = sizeof(*out),
												 .version = NXU_STATS_VERSION,
												 .thread_id = threads[i].thread_id,
												 .page_faults = threads[i].page_faults};
		kfree(threads);
		return count;
	}
	if (kind == NXU_STATS_THREAD) {
		*total = thread_total;
		uint32_t count = capacity < *total ? capacity : *total;
		nxu_stats_thread_t *out = records;
		for (uint32_t i = 0U; i < count; i++) {
			thread_snapshot_t *t = &threads[i];
			out[i] = (nxu_stats_thread_t){.struct_size = sizeof(*out),
										  .version = NXU_STATS_VERSION,
										  .thread_id = t->thread_id,
										  .task_uniqueid = t->task_uniqueid,
										  .cpu_us = t->run_us,
										  .last_cpu = t->last_cpu,
										  .mlfq_level = t->mlfq_level,
										  .flags = (t->idle ? NXU_STATS_THREAD_IDLE : 0U) |
												   (t->on_cpu ? NXU_STATS_THREAD_RUNNING : 0U) |
												   (t->waiting ? NXU_STATS_THREAD_WAITING : 0U)};
		}
		result = count;
	} else {
		proc_snapshot_t *procs = kmalloc(PROC_MAX * sizeof(*procs));
		if (procs == 0) {
			kfree(threads);
			return -NXU_SYS_E_NO_MEMORY;
		}
		*total = proc_snapshot(procs, PROC_MAX);
		uint32_t count = capacity < *total ? capacity : *total;
		nxu_stats_process_t *out = records;
		for (uint32_t i = 0U; i < count; i++) {
			proc_snapshot_t *p = &procs[i];
			out[i] = (nxu_stats_process_t){.struct_size = sizeof(*out),
										   .version = NXU_STATS_VERSION,
										   .uniqueid = p->uniqueid,
										   .pid = p->pid,
										   .ppid = p->ppid,
										   .state = p->state,
										   .flags = p->flags};
			memcpy(out[i].name, p->name, sizeof(out[i].name));
			out[i].name[sizeof(out[i].name) - 1U] = '\0';
			for (uint32_t j = 0U; j < thread_total; j++) {
				if (threads[j].task_uniqueid != p->uniqueid || threads[j].idle) continue;
				out[i].cpu_us += threads[j].run_us;
				out[i].threads++;
			}
		}
		kfree(procs);
		result = count;
	}
	kfree(threads);
	return result;
}
