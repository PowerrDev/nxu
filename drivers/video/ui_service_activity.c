/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/ui_service_activity.c
 *
 * See drivers/video/ui_service_activity.h. Processes and threads are copied
 * out under their own tables' locks, one table at a time (proc_snapshot,
 * thread_snapshot), and joined here on the process's uniqueid: nothing below
 * ever holds both locks.
 */

#include <drivers/video/ui_service_activity.h>

#if defined(NXU_UI_SERVICE)

#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/machine/timer.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/processor.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ABI's layout (UIService.h) is fixed the same on i386 and arm64. */
_Static_assert(sizeof(UIServiceProcessInfo) == 80U, "UIServiceProcessInfo layout");
_Static_assert(sizeof(UIServiceCpuInfo) == 32U, "UIServiceCpuInfo layout");
_Static_assert(offsetof(UIServiceActivity, cpus) == 56U, "UIServiceActivity layout");
_Static_assert(UI_SERVICE_ACTIVITY_NAME_MAX + 1U <= PROC_NAME_MAX, "process names fit");

/* Big enough for every slot of both tables; static, not on the UI's stack. */
static proc_snapshot_t g_activity_procs[PROC_MAX];
static thread_snapshot_t g_activity_threads[THREAD_MAX];

static uint32_t ui_service_activity_state(const proc_snapshot_t *proc, bool on_cpu, bool runnable)
{
	switch (proc->state) {
	case PROC_STATE_STOPPED:
		return UI_SERVICE_PROCESS_STATE_STOPPED;
	case PROC_STATE_RUNNABLE:
	case PROC_STATE_RUNNING:
		if (on_cpu) return UI_SERVICE_PROCESS_STATE_RUNNING;
		return runnable ? UI_SERVICE_PROCESS_STATE_RUNNABLE : UI_SERVICE_PROCESS_STATE_SLEEPING;
	default:
		return UI_SERVICE_PROCESS_STATE_OTHER;
	}
}

/*
 * The kernel is PID 0. The system's own processes are bootd (PID 1) and the
 * daemons it starts; there are no user accounts, so "user" means anything
 * else: programs started from a program.
 */
static uint32_t ui_service_activity_flags(const proc_snapshot_t *proc)
{
	if (proc->pid == PROC_PID_KERNEL) return UI_SERVICE_PROCESS_FLAG_KERNEL | UI_SERVICE_PROCESS_FLAG_SYSTEM;
	if ((proc->flags & PROC_FLAG_SYSTEM) != 0U || proc->pid == 1U || proc->ppid == 1U) return UI_SERVICE_PROCESS_FLAG_SYSTEM;
	return 0U;
}

uint32_t ui_service_get_activity(
	void *context,
	UIServiceActivity *activity,
	UIServiceProcessInfo *processes,
	uint32_t capacity,
	uint32_t *count_out
)
{
	(void)context;

	if (activity == 0 || count_out == 0 || (processes == 0 && capacity != 0U)) return UI_SERVICE_STATUS_INVALID_ARGUMENT;
	if (activity->struct_size < sizeof(UIServiceActivity)) return UI_SERVICE_STATUS_INVALID_ARGUMENT;

	*count_out = 0U;

	uint32_t cpus = processor_count();
	if (cpus > UI_SERVICE_ACTIVITY_CPU_MAX) cpus = UI_SERVICE_ACTIVITY_CPU_MAX;

	activity->cpu_count = cpus;
	activity->uptime_us = kconsole_uptime_us();
	activity->page_size = PMM_PAGE_SIZE;
	activity->total_pages = pmm_get_page_count();
	activity->free_pages = pmm_get_free_page_count();
	activity->heap_pages = heap_get_page_count();

	for (uint32_t cpu = 0U; cpu < UI_SERVICE_ACTIVITY_CPU_MAX; cpu++) {
		UIServiceCpuInfo *info = &activity->cpus[cpu];
		processor_t processor = cpu < cpus ? processor_by_id(cpu) : 0;

		/*
		 * Microseconds, measured at each context switch (see thread->run_us):
		 * the CPU's wall time and its busy part. Activity Monitor only takes
		 * ratios, and its "ticks per second" comes out as 1000000.
		 */
		uint64_t busy = processor != 0 ? processor->busy_us : 0ULL;
		uint64_t since = processor != 0 ? processor->busy_since_us : 0ULL;
		uint64_t now = timer_get_microseconds();

		if (since != 0ULL && now > since) busy += now - since;

		info->ticks = processor != 0 ? activity->uptime_us : 0ULL;
		info->busy_ticks = busy;
		info->context_switches = processor != 0 ? processor->context_switch_count : 0ULL;
		info->online = processor != 0 && processor_is_online(processor) ? 1U : 0U;
		info->reserved = 0U;
	}

	uint32_t proc_count = proc_snapshot(g_activity_procs, PROC_MAX);
	uint32_t thread_count = thread_snapshot(g_activity_threads, THREAD_MAX);
	uint32_t live_threads = 0U;

	for (uint32_t thread = 0U; thread < thread_count; thread++) {
		if (!g_activity_threads[thread].idle) live_threads++;
	}

	activity->process_count = proc_count;
	activity->thread_count = live_threads;

	uint32_t written = 0U;

	for (uint32_t index = 0U; index < proc_count && written < capacity; index++) {
		const proc_snapshot_t *proc = &g_activity_procs[index];
		UIServiceProcessInfo *info = &processes[written++];
		uint64_t ticks = 0ULL;
		uint32_t threads = 0U;
		uint32_t level = UINT32_MAX;
		uint32_t last_cpu = 0U;
		bool on_cpu = false;
		bool runnable = false;

		/* A CPU's idle thread belongs to the kernel task but is not work it does. */
		for (uint32_t thread = 0U; thread < thread_count; thread++) {
			const thread_snapshot_t *snapshot = &g_activity_threads[thread];

			if (snapshot->task_uniqueid != proc->uniqueid || snapshot->idle) continue;

			threads++;
			ticks += snapshot->run_us;
			if (snapshot->mlfq_level < level) level = snapshot->mlfq_level;
			if (snapshot->on_cpu) {
				on_cpu = true;
				last_cpu = snapshot->last_cpu;
			} else if (!on_cpu) {
				last_cpu = snapshot->last_cpu;
			}
			if (!snapshot->waiting) runnable = true;
		}

		info->uniqueid = proc->uniqueid;
		info->cpu_ticks = ticks;
		info->pid = proc->pid;
		info->ppid = proc->ppid;
		info->state = ui_service_activity_state(proc, on_cpu, runnable);
		info->flags = ui_service_activity_flags(proc);
		info->threads = threads;
		info->mlfq_level = level == UINT32_MAX ? 0U : level;
		info->last_cpu = last_cpu;

		uint32_t length = 0U;

		while (length < UI_SERVICE_ACTIVITY_NAME_MAX && proc->name[length] != '\0') {
			info->name[length] = proc->name[length];
			length++;
		}
		info->name[length] = '\0';
		info->name_length = length;
	}

	*count_out = written;
	return UI_SERVICE_STATUS_OK;
}

#endif
