#ifndef NXU_ACTIVITY_ABI_H
#define NXU_ACTIVITY_ABI_H

#include <stdint.h>

/* Fixed wire layout shared by legacy apps and the userland desktop. */
#define NXU_ACTIVITY_NAME_MAX 31U
#define NXU_ACTIVITY_CPU_MAX 8U

/* nxu_ui_process_info_t.state */
#define NXU_ACTIVITY_PROCESS_STATE_RUNNING 0U  /* one of its threads is on a CPU */
#define NXU_ACTIVITY_PROCESS_STATE_RUNNABLE 1U /* ready, waiting for a CPU */
#define NXU_ACTIVITY_PROCESS_STATE_SLEEPING 2U /* every thread is waiting */
#define NXU_ACTIVITY_PROCESS_STATE_STOPPED 3U
#define NXU_ACTIVITY_PROCESS_STATE_OTHER 4U /* being created or exiting */

/* nxu_ui_process_info_t.flags */
#define NXU_ACTIVITY_PROCESS_FLAG_KERNEL (1U << 0U)
#define NXU_ACTIVITY_PROCESS_FLAG_SYSTEM (1U << 1U)

typedef struct {
	uint64_t uniqueid;
	/* Scheduler ticks its live threads have run, all CPUs together. */
	uint64_t cpu_ticks;
	uint32_t pid;
	uint32_t ppid;
	uint32_t state;
	uint32_t flags;
	uint32_t threads;
	/* The highest-priority (lowest-numbered) MLFQ level of its threads. */
	uint32_t mlfq_level;
	/* The CPU its most recently scheduled thread ran on. */
	uint32_t last_cpu;
	uint32_t name_length;
	char name[NXU_ACTIVITY_NAME_MAX + 1U];
} nxu_ui_process_info_t;

typedef struct {
	/* Scheduler ticks this CPU took, and how many found it running something. */
	uint64_t ticks;
	uint64_t busy_ticks;
	uint64_t context_switches;
	uint32_t online;
	uint32_t reserved;
} nxu_ui_cpu_info_t;

typedef struct {
	uint32_t struct_size;
	uint32_t cpu_count;
	uint64_t uptime_us;
	uint64_t page_size;
	uint64_t total_pages;
	uint64_t free_pages;
	uint64_t heap_pages;
	uint32_t process_count;
	uint32_t thread_count;
	nxu_ui_cpu_info_t cpus[NXU_ACTIVITY_CPU_MAX];
} nxu_ui_activity_t;

_Static_assert(sizeof(nxu_ui_process_info_t) == 80, "activity process ABI");
_Static_assert(sizeof(nxu_ui_activity_t) == 312, "activity system ABI");

#endif
