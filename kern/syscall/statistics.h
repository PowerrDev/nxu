#ifndef NXU_STATISTICS_ABI_H
#define NXU_STATISTICS_ABI_H

#include <stdint.h>

#define NXU_STATS_VERSION 1U
#define NXU_STATS_SYSTEM 1U
#define NXU_STATS_CPU 2U
#define NXU_STATS_PROCESS 3U
#define NXU_STATS_THREAD 4U
#define NXU_STATS_PROCESS_MEMORY 5U
#define NXU_STATS_CPU_EVENTS 6U
#define NXU_STATS_VIRTIO 7U
#define NXU_STATS_THREAD_EVENTS 8U
#define NXU_STATS_MEMORY_KERNEL 1U
#define NXU_STATS_NAME_MAX 32U
#define NXU_STATS_PROCESS_UNUSED 0U
#define NXU_STATS_PROCESS_EMBRYO 1U
#define NXU_STATS_PROCESS_RUNNABLE 2U
#define NXU_STATS_PROCESS_RUNNING 3U
#define NXU_STATS_PROCESS_STOPPED 4U
#define NXU_STATS_PROCESS_ZOMBIE 5U
#define NXU_STATS_PROCESS_DEAD 6U
#define NXU_STATS_PROCESS_SYSTEM 1U
#define NXU_STATS_PROCESS_EXITING 2U
#define NXU_STATS_PROCESS_REAPED 4U
#define NXU_STATS_PROCESS_EXECING 8U
#define NXU_STATS_PROCESS_TRACED 16U
#define NXU_STATS_THREAD_IDLE 1U
#define NXU_STATS_THREAD_RUNNING 2U
#define NXU_STATS_THREAD_WAITING 4U

/* Fixed-width, identical layout on i386 and ARM64. All times are microseconds.
 * SYSTEM/CPU counters last until reboot; thread counters last until destruction.
 * Process CPU time sums live non-idle threads, and can decrease on thread exit.
 * Tables are independently sampled. Join on uniqueid, never recyclable PID.
 * capacity=0 queries total only. Successful calls return count, set total to the
 * table size and copy min(capacity,total) records. Retry larger if truncated;
 * table membership may change between calls. No pointers or kernel addresses
 * are included in output. Unsupported versions/sizes return INVALID_ARGUMENT.
 */
typedef struct {
	uint32_t struct_size, version, kind, record_size;
	uint64_t records;
	uint32_t capacity, count, total, reserved;
} nxu_stats_request_t;

typedef struct {
	uint32_t struct_size, version;
	uint64_t sampled_us, total_pages, free_pages, heap_pages;
	uint32_t page_size, cpu_count, process_count, thread_count;
} nxu_stats_system_t;

typedef struct {
	uint32_t struct_size, version;
	uint64_t sampled_us, busy_us, context_switches, dispatches, preemptions;
	uint64_t quantum_expirations;
	uint32_t cpu_id, online, run_queue_depth, reserved;
} nxu_stats_cpu_t;

typedef struct {
	uint32_t struct_size, version;
	uint64_t uniqueid, cpu_us;
	uint32_t pid, ppid, state, flags, threads, reserved;
	char name[NXU_STATS_NAME_MAX];
} nxu_stats_process_t;

typedef struct {
	uint32_t struct_size, version;
	uint64_t thread_id, task_uniqueid, cpu_us;
	uint32_t last_cpu, mlfq_level, flags, reserved;
} nxu_stats_thread_t;

/* Resident counts are mappings, not unique physical ownership. Shared and
 * copy-on-write pages count in each process. Anonymous reservations include
 * resident anonymous pages; they exclude ELF and SHM address ranges. */
typedef struct {
	uint32_t struct_size, version;
	uint64_t uniqueid, resident_pages, shared_pages, anon_reserved_pages;
	uint64_t page_table_pages, page_faults;
	uint32_t pid, flags;
} nxu_stats_process_memory_t;

/* Hardware faults and acknowledged interrupts (timer and IPIs included).
 * User faults are attributed to their thread/task before resolution; kernel
 * faults count only per CPU. Software user-copy resolutions are excluded. */
typedef struct {
	uint32_t struct_size, version, cpu_id, reserved;
	uint64_t page_faults, interrupts;
} nxu_stats_cpu_events_t;

typedef struct {
	uint32_t struct_size, version;
	uint64_t thread_id, page_faults;
} nxu_stats_thread_events_t;

/* Aggregate split-virtqueue traffic since boot, across devices. Used bytes
 * are device-reported completion lengths, not disk/network transfer bytes.
 * Exhaustion counts descriptor allocation failures, not device errors. */
typedef struct {
	uint32_t struct_size, version;
	uint64_t submissions, completions, used_bytes, rejected_submissions;
	uint64_t invalid_completions, descriptor_exhaustions, live_queues;
} nxu_stats_virtio_t;

_Static_assert(sizeof(nxu_stats_process_memory_t) == 64U, "stats memory ABI");
_Static_assert(sizeof(nxu_stats_cpu_events_t) == 32U, "stats CPU events ABI");
_Static_assert(sizeof(nxu_stats_thread_events_t) == 24U, "stats thread events ABI");
_Static_assert(sizeof(nxu_stats_virtio_t) == 64U, "stats VirtIO ABI");
_Static_assert(sizeof(nxu_stats_request_t) == 40U, "stats request ABI");
_Static_assert(sizeof(nxu_stats_system_t) == 56U, "stats system ABI");
_Static_assert(sizeof(nxu_stats_cpu_t) == 72U, "stats CPU ABI");
_Static_assert(sizeof(nxu_stats_process_t) == 80U, "stats process ABI");
_Static_assert(sizeof(nxu_stats_thread_t) == 48U, "stats thread ABI");
#endif
