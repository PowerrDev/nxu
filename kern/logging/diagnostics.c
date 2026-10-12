/* Actual kernel state and syscall activity; no synthetic progress messages. */
#include <kern/logging/diagnostics.h>
#include <kern/boot/boot_args.h>
#include <kern/console/console.h>
#include <kern/process/statistics.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/processor.h>
#include <kern/ipc/ipc_port.h>

#define CALL_COUNT (NXU_SYS_READ_POLL + 1U)
static const char *const g_names[CALL_COUNT] = {
	[NXU_SYS_NETWORK] = "network",
	[NXU_SYS_DUP2] = "dup2",
	[NXU_SYS_READ_POLL] = "read_poll",
	[NXU_SYS_EXIT] = "exit",
	[NXU_SYS_WRITE] = "write",
	[NXU_SYS_GET_VERSION] = "get_version",
	[NXU_SYS_OPEN] = "open",
	[NXU_SYS_READ] = "read",
	[NXU_SYS_CLOSE] = "close",
	[NXU_SYS_SPAWN] = "spawn",
	[NXU_SYS_WAITPID] = "waitpid",
	[NXU_SYS_GETPID] = "getpid",
	[NXU_SYS_YIELD] = "yield",
	[NXU_SYS_GET_BOOT_ARGS] = "get_boot_args",
	[NXU_SYS_KLOG_READ] = "klog_read",
	[NXU_SYS_UNLINK] = "unlink",
	[NXU_SYS_SYNC] = "sync",
	[NXU_SYS_UPTIME_US] = "uptime_us",
	[NXU_SYS_READDIR] = "readdir",
	[NXU_SYS_RECOVERY_DISPLAY_INFO] = "recovery_display_info",
	[NXU_SYS_RECOVERY_PRESENT] = "recovery_present",
	[NXU_SYS_RECOVERY_INPUT] = "recovery_input",
	[NXU_SYS_EVENT_WAIT] = "event_wait",
	[NXU_SYS_IPC_RECEIVE_TIMED] = "ipc_receive_timed",
	[NXU_SYS_RECOVERY_BLOCK_PARTITION_DELETE] = "partition_delete",
	[NXU_SYS_RMDIR] = "rmdir",
	[NXU_SYS_SAP_REMOVE_TREE] = "sap_remove_tree",
	[NXU_SYS_SYSTEM_RESET] = "system_reset",
	[NXU_SYS_SEEK] = "seek",
	[NXU_SYS_STAT] = "stat",
	[NXU_SYS_MKDIR] = "mkdir",
	[NXU_SYS_RECOVERY_FS_MOUNT_INFO] = "recovery_fs_mount_info",
	[NXU_SYS_RECOVERY_FS_MOUNT] = "recovery_fs_mount",
	[NXU_SYS_RECOVERY_FS_UNMOUNT] = "recovery_fs_unmount",
	[NXU_SYS_RECOVERY_BLOCK_INFO] = "recovery_block_info",
	[NXU_SYS_RECOVERY_FS_SPACE_INFO] = "recovery_fs_space_info",
	[NXU_SYS_RECOVERY_BLOCK_LAYOUT_INFO] = "recovery_block_layout_info",
	[NXU_SYS_RECOVERY_BLOCK_PARTITION_INFO] = "recovery_block_partition_info",
	[NXU_SYS_RECOVERY_BLOCK_HEALTH_INFO] = "recovery_block_health_info",
	[NXU_SYS_RECOVERY_BLOCK_VERIFY] = "recovery_block_verify",
	[NXU_SYS_IPC_PORT_ALLOCATE] = "ipc_port_allocate",
	[NXU_SYS_IPC_PORT_DEALLOCATE] = "ipc_port_deallocate",
	[NXU_SYS_IPC_BOOTSTRAP_PORT] = "ipc_bootstrap_port",
	[NXU_SYS_IPC_SEND] = "ipc_send",
	[NXU_SYS_IPC_RECEIVE] = "ipc_receive",
	[NXU_SYS_IPC_REGISTER_BOOTSTRAP] = "ipc_register_bootstrap",
	[NXU_SYS_DISPLAY_CLAIM] = "display_claim",
	[NXU_SYS_SHM_CREATE] = "shm_create",
	[NXU_SYS_SHM_MAP] = "shm_map",
	[NXU_SYS_MMAP] = "mmap",
	[NXU_SYS_MUNMAP] = "munmap",
	[NXU_SYS_THREAD_CREATE] = "thread_create",
	[NXU_SYS_THREAD_EXIT] = "thread_exit",
	[NXU_SYS_THREAD_SELF] = "thread_self",
	[NXU_SYS_SOCKET_LISTEN] = "socket_listen",
	[NXU_SYS_SOCKET_CONNECT] = "socket_connect",
	[NXU_SYS_SOCKET_ACCEPT] = "socket_accept",
	[NXU_SYS_FORK] = "fork",
	[NXU_SYS_EXEC] = "exec",
	[NXU_SYS_KILL] = "kill",
	[NXU_SYS_SIGACTION] = "sigaction",
	[NXU_SYS_SIGPROCMASK] = "sigprocmask",
	[NXU_SYS_SIGRETURN] = "sigreturn",
	[NXU_SYS_GETPPID] = "getppid",
	[NXU_SYS_WAIT] = "wait",
	[NXU_SYS_IPC_RECEIVE_WAIT] = "ipc_receive_wait",
	[NXU_SYS_GET_CAPS] = "get_caps",
	[NXU_SYS_IOCTL] = "ioctl",
	[NXU_SYS_GETCPU] = "getcpu",
	[NXU_SYS_SETAFFINITY] = "setaffinity",
	[NXU_SYS_SHM_UNMAP] = "shm_unmap",
	[NXU_SYS_SHM_WITHDRAW] = "shm_withdraw",
	[NXU_SYS_UI_CONNECT] = "ui_connect",
	[NXU_SYS_UI_RECEIVE] = "ui_receive",
	[NXU_SYS_UI_SUBMIT] = "ui_submit",
	[NXU_SYS_UI_CONTROL] = "ui_control",
	[NXU_SYS_SLEEP_US] = "sleep_us",
	[NXU_SYS_STATISTICS] = "statistics",
	[NXU_SYS_CRASH] = "crash",
	[NXU_SYS_DYRLD] = "dyrld",
	[NXU_SYS_SAP_DELETE] = "sap_delete",
	[NXU_SYS_GUI_HOST] = "gui_host",
	[NXU_SYS_SHM_MAP_SIZED] = "shm_map_sized",
	[NXU_SYS_IPC_RECEIVE_PEER] = "ipc_receive_peer",
	[NXU_SYS_PROCESS_IDENTITY] = "process_identity",
	[NXU_SYS_GUI_WINDOW_HANDLE] = "gui_window_handle",
};
static uint32_t g_calls[NXU_MAX_CPUS][CALL_COUNT];
static uint32_t g_errors[NXU_MAX_CPUS][CALL_COUNT];
static proc_snapshot_t g_processes[PROC_MAX];
static thread_snapshot_t g_threads[THREAD_MAX];
static nxu_stats_process_memory_t g_memory[PROC_MAX];
static bool g_started;

static bool
trace_call(uint64_t call)
{
	if (!kconsole_verbose() || !boot_arg_bool("log.trace", true)) return false;
	switch (call) {
	/* bootd polls each live child every 10 ms; trace summaries retain counts. */
	case NXU_SYS_WAITPID:
	case NXU_SYS_WRITE:
	case NXU_SYS_READ:
	case NXU_SYS_YIELD:
	case NXU_SYS_GETPID:
	case NXU_SYS_GETPPID:
	case NXU_SYS_GETCPU:
	case NXU_SYS_THREAD_SELF:
	case NXU_SYS_UPTIME_US:
	case NXU_SYS_KLOG_READ:
	case NXU_SYS_UI_RECEIVE:
	case NXU_SYS_UI_SUBMIT:
	case NXU_SYS_SLEEP_US:
	case NXU_SYS_STATISTICS:
	case NXU_SYS_IPC_RECEIVE:
	case NXU_SYS_IPC_RECEIVE_WAIT:
	case NXU_SYS_IPC_RECEIVE_PEER:
	case NXU_SYS_PROCESS_IDENTITY:
	case NXU_SYS_RECOVERY_PRESENT:
	case NXU_SYS_RECOVERY_INPUT:
		return false;
	default:
		return true;
	}
}

static uint32_t
call_slot(uint64_t call)
{
	return call < CALL_COUNT && g_names[call] != 0 ? (uint32_t)call : 0U;
}

void
diagnostics_syscall_begin(const syscall_request_t *request)
{
	if (!kconsole_verbose()) return;
	processor_t cpu = current_processor();
	uint32_t id = cpu != 0 ? cpu->cpu_id : 0U;
	if (id < NXU_MAX_CPUS) __atomic_fetch_add(&g_calls[id][call_slot(request->number)], 1U, __ATOMIC_RELAXED);
	if (trace_call(request->number))
		kverbosef("syscall: cpu=%u pid=%u tid=%llu %s(%llu) args=%llx,%llx,%llx,%llx,%llx,%llx enter\n", id,
				  proc_selfpid(), (unsigned long long)thread_tid(current_thread()),
				  g_names[call_slot(request->number)] != 0 ? g_names[call_slot(request->number)] : "unknown",
				  (unsigned long long)request->number, (unsigned long long)request->arguments[0],
				  (unsigned long long)request->arguments[1], (unsigned long long)request->arguments[2],
				  (unsigned long long)request->arguments[3], (unsigned long long)request->arguments[4],
				  (unsigned long long)request->arguments[5]);
}

void
diagnostics_syscall_end(const syscall_request_t *request, syscall_result_t result)
{
	if (!kconsole_verbose()) return;
	processor_t cpu = current_processor();
	uint32_t id = cpu != 0 ? cpu->cpu_id : 0U;
	if (result.action == SYSCALL_ACTION_RETURN && result.value >= UINT64_MAX - 4094ULL && id < NXU_MAX_CPUS)
		__atomic_fetch_add(&g_errors[id][call_slot(request->number)], 1U, __ATOMIC_RELAXED);
	if (trace_call(request->number))
		kverbosef("syscall: cpu=%u pid=%u tid=%llu %s result=%llx action=%u\n", id, proc_selfpid(),
				  (unsigned long long)thread_tid(current_thread()),
				  g_names[call_slot(request->number)] != 0 ? g_names[call_slot(request->number)] : "unknown",
				  (unsigned long long)result.value, (unsigned)result.action);
}

static void
diagnostics_sample(void)
{
	uint32_t total;
	nxu_stats_system_t system;
	statistics_snapshot(NXU_STATS_SYSTEM, &system, 1U, &total);
	kverbosef("memory: uptime_us=%llu physical_pages=%llu free=%llu heap=%llu processes=%u threads=%u\n",
			  (unsigned long long)system.sampled_us, (unsigned long long)system.total_pages,
			  (unsigned long long)system.free_pages, (unsigned long long)system.heap_pages, system.process_count,
			  system.thread_count);
	nxu_stats_cpu_events_t events[NXU_MAX_CPUS];
	int64_t count = statistics_snapshot(NXU_STATS_CPU_EVENTS, events, NXU_MAX_CPUS, &total);
	for (uint32_t i = 0; count > 0 && i < (uint32_t)count; i++) {
		nxu_stats_cpu_t cpu;
		statistics_cpu_snapshot(i, &cpu);
		kverbosef("sched: cpu=%u online=%u busy_us=%llu runq=%u switches=%llu dispatches=%llu preemptions=%llu "
				  "quantum=%llu faults=%llu irqs=%llu\n",
				  i, cpu.online, (unsigned long long)cpu.busy_us, cpu.run_queue_depth,
				  (unsigned long long)cpu.context_switches, (unsigned long long)cpu.dispatches,
				  (unsigned long long)cpu.preemptions, (unsigned long long)cpu.quantum_expirations,
				  (unsigned long long)events[i].page_faults, (unsigned long long)events[i].interrupts);
	}
	nxu_stats_virtio_t virtio;
	statistics_snapshot(NXU_STATS_VIRTIO, &virtio, 1U, &total);
	kverbosef("virtqueue: queues=%llu submitted=%llu completed=%llu used_bytes=%llu rejected=%llu invalid=%llu "
			  "exhausted=%llu\n",
			  (unsigned long long)virtio.live_queues, (unsigned long long)virtio.submissions,
			  (unsigned long long)virtio.completions, (unsigned long long)virtio.used_bytes,
			  (unsigned long long)virtio.rejected_submissions, (unsigned long long)virtio.invalid_completions,
			  (unsigned long long)virtio.descriptor_exhaustions);
	kverbosef("ipc: outstanding_ports=%llu\n", (unsigned long long)ipc_port_outstanding_count());
	uint32_t processes = proc_snapshot(g_processes, PROC_MAX);
	uint32_t memories = proc_memory_snapshot(g_memory, PROC_MAX);
	if (memories > PROC_MAX) memories = PROC_MAX;
	for (uint32_t i = 0; i < processes; i++) {
		proc_snapshot_t *p = &g_processes[i];
		for (uint32_t c = 0; c < PROC_NAME_MAX; c++)
			if ((unsigned char)p->name[c] < 32U || (unsigned char)p->name[c] > 126U) {
				if (p->name[c] == 0) break;
				p->name[c] = '?';
			}
		p->name[PROC_NAME_MAX - 1U] = 0;
		kverbosef("proc: pid=%u ppid=%u uniqueid=%llu name=%s state=%u flags=%x threads=%u\n", p->pid, p->ppid,
				  (unsigned long long)p->uniqueid, p->name, (unsigned)p->state, p->flags, p->thread_count);
	}
	for (uint32_t i = 0; i < memories; i++) {
		nxu_stats_process_memory_t *m = &g_memory[i];
		kverbosef("vm: pid=%u uniqueid=%llu resident_pages=%llu shared=%llu anon_reserved=%llu tables=%llu faults=%llu "
				  "flags=%x\n",
				  m->pid, (unsigned long long)m->uniqueid, (unsigned long long)m->resident_pages,
				  (unsigned long long)m->shared_pages, (unsigned long long)m->anon_reserved_pages,
				  (unsigned long long)m->page_table_pages, (unsigned long long)m->page_faults, m->flags);
	}
	uint32_t threads = thread_snapshot(g_threads, THREAD_MAX);
	for (uint32_t i = 0; i < threads; i++) {
		thread_snapshot_t *t = &g_threads[i];
		kverbosef("thread: tid=%llu task=%llu cpu=%u cpu_us=%llu faults=%llu mlfq=%u idle=%u running=%u waiting=%u\n",
				  (unsigned long long)t->thread_id, (unsigned long long)t->task_uniqueid, t->last_cpu,
				  (unsigned long long)t->run_us, (unsigned long long)t->page_faults, t->mlfq_level, t->idle, t->on_cpu,
				  t->waiting);
	}
	for (uint32_t call = 0; call < CALL_COUNT; call++) {
		uint64_t calls = 0, errors = 0;
		for (uint32_t cpu = 0; cpu < NXU_MAX_CPUS; cpu++) {
			calls += __atomic_exchange_n(&g_calls[cpu][call], 0U, __ATOMIC_RELAXED);
			errors += __atomic_exchange_n(&g_errors[cpu][call], 0U, __ATOMIC_RELAXED);
		}
		if (calls || errors)
			kverbosef("syscall: interval %s calls=%llu errors=%llu\n", g_names[call] != 0 ? g_names[call] : "unknown",
					  (unsigned long long)calls, (unsigned long long)errors);
	}
}

static void
diagnostics_worker(void *parameter)
{
	uint32_t interval = (uint32_t)(uintptr_t)parameter;
	for (;;) {
		if (kconsole_verbose()) diagnostics_sample();
		if (!sched_sleep_us((uint64_t)interval * 1000ULL)) sched_yield();
	}
}

void
diagnostics_start(void)
{
	if (g_started || !kconsole_verbose()) return;
	uint32_t interval = boot_arg_uint32("log.stats_ms", 1000U);
	if (interval == 0U) return;
	if (interval < 250U) interval = 250U;
	if (interval > 60000U) interval = 60000U;
	thread_t thread = 0;
	nxu_cpuset_t affinity;
	cpuset_clear(&affinity);
	cpuset_add(&affinity, 0U);
	if (!kernel_thread_create(proc_task(proc_kernel()), diagnostics_worker, (void *)(uintptr_t)interval, &thread)) {
		kputln("diagnostics: could not create logger thread");
		return;
	}
	if (!thread_set_affinity(thread, &affinity) || !sched_thread_start(thread)) {
		thread_terminate(thread);
		thread_reap(thread);
		thread_deallocate(thread);
		kputln("diagnostics: could not schedule logger thread");
		return;
	}
	thread_deallocate(thread);
	g_started = true;
	kverbosef("diagnostics: live kernel snapshots every %u ms\n", interval);
	kverbosef("diagnostics: syscall traces=%u\n", boot_arg_bool("log.trace", true));
}
