#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/sched_prism/sched.h>

#include <mach/machine/cpu.h>

#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	volatile uint32_t value;
} proc_lock_t;

static proc_lock_t g_proc_lock;

static struct proc g_proc_slots[PROC_MAX];
static bool g_proc_slot_used[PROC_MAX];

static proc_t g_allproc_head;
static proc_t g_allproc_tail;

static proc_t g_zombproc_head;
static proc_t g_zombproc_tail;

static proc_t g_kernproc;
static proc_t g_initproc;
static proc_t g_current_proc;

static proc_id_t g_next_pid = 1U;
static proc_uniqueid_t g_next_uniqueid = 1ULL;
static proc_idversion_t g_next_idversion = 1U;

static uint32_t g_proc_count;
static uint32_t g_zombie_count;

static bool g_proc_initialized;

static void proc_lock(proc_lock_t *lock)
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

static void proc_unlock(proc_lock_t *lock)
{
	__atomic_store_n(
		&lock->value,
		0U,
		__ATOMIC_RELEASE
	);
}

static void proc_copy_name(
	char destination[PROC_NAME_MAX],
	const char *source
)
{
	uint32_t index = 0U;

	if (source != 0) {
		while (
			index < PROC_NAME_MAX - 1U &&
			source[index] != '\0'
		) {
			destination[index] = source[index];
			index++;
		}
	}

	destination[index] = '\0';
	index++;

	while (index < PROC_NAME_MAX) {
		destination[index] = '\0';
		index++;
	}
}

static proc_t proc_allocate_slot_locked(void)
{
	for (
		uint32_t index = 0U;
		index < PROC_MAX;
		index++
	) {
		if (g_proc_slot_used[index]) continue;

		g_proc_slot_used[index] = true;

		g_proc_slots[index] = (struct proc) {
			.p_list_prev = 0,
			.p_list_next = 0,
			.p_pptr = 0,
			.p_children = 0,
			.p_sibling_prev = 0,
			.p_sibling_next = 0,
			.p_task = { 0 },
			.p_fd = { 0 },
			.p_ipc = { 0 },
			.p_ipc_bootstrap_name = IPC_SPACE_NAME_INVALID,
			.p_ident = {
				.pid = PROC_PID_INVALID,
				.uniqueid = 0ULL,
				.idversion = 0U
			},
			.p_stat = PROC_STATE_EMBRYO,
			.p_flag = PROC_FLAG_NONE,
			.p_refcount = 0U,
			.p_childrencnt = 0U,
			.p_xstat = 0,
			.p_comm = { 0 },
			.p_slot = index
		};

		filedesc_init(&g_proc_slots[index].p_fd);
		ipc_space_init(&g_proc_slots[index].p_ipc);

		return &g_proc_slots[index];
	}

	return 0;
}

static void proc_release_slot_locked(proc_t proc)
{
	if (proc == 0 || proc->p_slot >= PROC_MAX) return;

	uint32_t slot = proc->p_slot;

	if (&g_proc_slots[slot] != proc) return;

	g_proc_slot_used[slot] = false;
	g_proc_slots[slot] = (struct proc) { 0 };
}

static bool proc_pid_in_use_locked(proc_id_t pid)
{
	for (
		uint32_t index = 0U;
		index < PROC_MAX;
		index++
	) {
		if (!g_proc_slot_used[index]) continue;

		if (g_proc_slots[index].p_ident.pid == pid) {
			return true;
		}
	}

	return false;
}

static proc_id_t proc_allocate_pid_locked(void)
{
	for (
		uint32_t attempt = 0U;
		attempt < PROC_PID_MAX;
		attempt++
	) {
		if (
			g_next_pid == PROC_PID_KERNEL ||
			g_next_pid > PROC_PID_MAX
		) {
			g_next_pid = 1U;
		}

		proc_id_t candidate = g_next_pid;
		g_next_pid++;

		if (!proc_pid_in_use_locked(candidate)) {
			return candidate;
		}
	}

	return PROC_PID_INVALID;
}

static proc_uniqueid_t proc_allocate_uniqueid_locked(void)
{
	proc_uniqueid_t uniqueid = g_next_uniqueid++;

	if (uniqueid == 0ULL) {
		uniqueid = g_next_uniqueid++;
	}

	return uniqueid;
}

static proc_idversion_t proc_allocate_idversion_locked(void)
{
	proc_idversion_t version = g_next_idversion++;

	if (version == 0U) {
		version = g_next_idversion++;
	}

	return version;
}

static void proc_all_insert_locked(proc_t proc)
{
	proc->p_list_prev = g_allproc_tail;
	proc->p_list_next = 0;

	if (g_allproc_tail != 0) {
		g_allproc_tail->p_list_next = proc;
	} else {
		g_allproc_head = proc;
	}

	g_allproc_tail = proc;
}

static void proc_all_remove_locked(proc_t proc)
{
	if (proc->p_list_prev != 0) {
		proc->p_list_prev->p_list_next = proc->p_list_next;
	} else {
		g_allproc_head = proc->p_list_next;
	}

	if (proc->p_list_next != 0) {
		proc->p_list_next->p_list_prev = proc->p_list_prev;
	} else {
		g_allproc_tail = proc->p_list_prev;
	}

	proc->p_list_prev = 0;
	proc->p_list_next = 0;
}

static void proc_zombie_insert_locked(proc_t proc)
{
	proc->p_list_prev = g_zombproc_tail;
	proc->p_list_next = 0;

	if (g_zombproc_tail != 0) {
		g_zombproc_tail->p_list_next = proc;
	} else {
		g_zombproc_head = proc;
	}

	g_zombproc_tail = proc;
}

static void proc_zombie_remove_locked(proc_t proc)
{
	if (proc->p_list_prev != 0) {
		proc->p_list_prev->p_list_next = proc->p_list_next;
	} else {
		g_zombproc_head = proc->p_list_next;
	}

	if (proc->p_list_next != 0) {
		proc->p_list_next->p_list_prev = proc->p_list_prev;
	} else {
		g_zombproc_tail = proc->p_list_prev;
	}

	proc->p_list_prev = 0;
	proc->p_list_next = 0;
}

static void proc_child_insert_locked(
	proc_t parent,
	proc_t child
)
{
	child->p_pptr = parent;

	child->p_sibling_prev = 0;
	child->p_sibling_next = parent->p_children;

	if (parent->p_children != 0) {
		parent->p_children->p_sibling_prev = child;
	}

	parent->p_children = child;
	parent->p_childrencnt++;
}

static void proc_child_remove_locked(proc_t child)
{
	proc_t parent = child->p_pptr;

	if (parent == 0) return;


	if (child->p_sibling_prev != 0) {
		child->p_sibling_prev->p_sibling_next = child->p_sibling_next;
	} else {
		parent->p_children = child->p_sibling_next;
	}

	if (child->p_sibling_next != 0) {
		child->p_sibling_next->p_sibling_prev = child->p_sibling_prev;
	}

	if (parent->p_childrencnt != 0U) {
		parent->p_childrencnt--;
	}

	child->p_pptr = 0;
	child->p_sibling_prev = 0;
	child->p_sibling_next = 0;
}

static proc_t proc_find_live_locked(proc_id_t pid)
{
	for (
		proc_t proc = g_allproc_head;
		proc != 0;
		proc = proc->p_list_next
	) {
		if (proc->p_ident.pid == pid) {
			return proc;
		}
	}

	return 0;
}

static proc_t proc_find_zombie_locked(proc_id_t pid)
{
	for (
		proc_t proc = g_zombproc_head;
		proc != 0;
		proc = proc->p_list_next
	) {
		if (proc->p_ident.pid == pid) {
			return proc;
		}
	}

	return 0;
}

static bool proc_reference_locked(proc_t proc)
{
	if (
		proc == 0 ||
		proc->p_refcount == UINT32_MAX
	) {
		return false;
	}

	proc->p_refcount++;
	return true;
}

static void proc_reparent_children_locked(proc_t proc)
{
	proc_t new_parent = g_initproc;

	if (
		new_parent == 0 ||
		new_parent == proc ||
		new_parent->p_stat == PROC_STATE_ZOMBIE ||
		new_parent->p_stat == PROC_STATE_DEAD
	) {
		new_parent = g_kernproc;
	}

	while (proc->p_children != 0) {
		proc_t child = proc->p_children;

		proc_child_remove_locked(child);
		proc_child_insert_locked(
			new_parent,
			child
		);
	}
}

bool proc_bootstrap(void)
{
	proc_lock(&g_proc_lock);

	if (g_proc_initialized) {
		proc_unlock(&g_proc_lock);
		return true;
	}

	proc_t kernel_proc = proc_allocate_slot_locked();

	if (kernel_proc == 0) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	kernel_proc->p_ident.pid = PROC_PID_KERNEL;

	kernel_proc->p_ident.uniqueid = 0ULL;
	kernel_proc->p_ident.idversion = 0U;

	kernel_proc->p_stat = PROC_STATE_RUNNING;

	kernel_proc->p_flag = PROC_FLAG_SYSTEM;

	kernel_proc->p_refcount = 1U;

	proc_copy_name(
		kernel_proc->p_comm,
		"kernel_task"
	);

	if (!task_init_kernel(
		&kernel_proc->p_task,
		0ULL,
		kernel_proc
	)) {
		proc_release_slot_locked(kernel_proc);
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc_all_insert_locked(kernel_proc);

	g_kernproc = kernel_proc;
	g_current_proc = kernel_proc;

	g_proc_count = 1U;
	g_zombie_count = 0U;

	g_proc_initialized = true;

	proc_unlock(&g_proc_lock);
	return true;
}

proc_t proc_kernel(void)
{
	return g_kernproc;
}

proc_t proc_initproc(void)
{
	return g_initproc;
}

bool proc_create_user(
	proc_t parent,
	const char *name,
	uint64_t entry,
	uint64_t stack,
	proc_t *result
)
{
	if (
		parent == 0 ||
		result == 0 ||
		entry == 0ULL ||
		stack == 0ULL
	) {
		return false;
	}

	*result = 0;

	proc_lock(&g_proc_lock);

	if (!g_proc_initialized) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	if (
		parent->p_stat == PROC_STATE_ZOMBIE ||
		parent->p_stat == PROC_STATE_DEAD
	) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc_t proc = proc_allocate_slot_locked();

	if (proc == 0) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc_id_t pid = proc_allocate_pid_locked();

	if (pid == PROC_PID_INVALID) {
		proc_release_slot_locked(proc);
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc->p_ident.pid = pid;

	proc->p_ident.uniqueid = proc_allocate_uniqueid_locked();

	proc->p_ident.idversion = proc_allocate_idversion_locked();

	proc->p_stat = PROC_STATE_EMBRYO;
	proc->p_refcount = 1U;

	proc_copy_name(
		proc->p_comm,
		name
	);

	if (!task_init_user(
		&proc->p_task,
		proc->p_ident.uniqueid,
		proc,
		entry,
		stack
	)) {
		proc_release_slot_locked(proc);
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc_child_insert_locked(
		parent,
		proc
	);

	proc_all_insert_locked(proc);

	g_proc_count++;

	if (
		g_initproc == 0 &&
		proc->p_ident.pid == 1U
	) {
		g_initproc = proc;
	}

	*result = proc;

	proc_unlock(&g_proc_lock);
	return true;
}

bool proc_fork(proc_t parent, proc_t *result)
{
	if (parent == 0 || result == 0) return false;

	*result = 0;

	proc_lock(&g_proc_lock);

	if (
		!g_proc_initialized ||
		parent == g_kernproc ||
		parent->p_stat == PROC_STATE_ZOMBIE ||
		parent->p_stat == PROC_STATE_DEAD
	) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc_t proc = proc_allocate_slot_locked();

	if (proc == 0) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc_id_t pid = proc_allocate_pid_locked();

	if (pid == PROC_PID_INVALID) {
		proc_release_slot_locked(proc);
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc->p_ident.pid = pid;
	proc->p_ident.uniqueid = proc_allocate_uniqueid_locked();
	proc->p_ident.idversion = proc_allocate_idversion_locked();

	proc->p_stat = PROC_STATE_EMBRYO;
	proc->p_refcount = 1U;

	proc_copy_name(proc->p_comm, parent->p_comm);

	if (!task_fork(&proc->p_task, &parent->p_task, proc->p_ident.uniqueid, proc)) {
		proc_release_slot_locked(proc);
		proc_unlock(&g_proc_lock);
		return false;
	}

	if (
		!filedesc_fork(&proc->p_fd, &parent->p_fd) ||
		!ipc_space_fork(&proc->p_ipc, &parent->p_ipc)
	) {
		/* Undo everything task_fork built; the embryo never became visible. */
		(void)task_terminate(&proc->p_task);
		filedesc_close_all(&proc->p_fd);
		ipc_space_close_all(&proc->p_ipc);
		proc_release_slot_locked(proc);
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc->p_ipc_bootstrap_name = parent->p_ipc_bootstrap_name;
	signal_inherit(proc, parent);

	proc_child_insert_locked(parent, proc);
	proc_all_insert_locked(proc);

	g_proc_count++;

	proc_unlock(&g_proc_lock);

	/*
	 * Start the child's first thread. Same steps as loader_start_process:
	 * a kernel stack, then runnable, then handed to the scheduler.
	 */
	task_t task = proc_task(proc);
	thread_t thread = task_first_thread_ref(task);
	bool started = false;

	if (thread != 0) {
		started =
			thread_stack_alloc(thread) &&
			proc_make_runnable(proc) &&
			proc_mark_running(proc) &&
			sched_thread_start(thread);

		thread_deallocate(thread);
	}

	if (!started) {
		/* Already visible in the parent's child list: end it and let the
		 * parent's next waitpid clean it up like any other exit. */
		(void)proc_exit(proc, 127ULL);
		return false;
	}

	*result = proc;
	return true;
}

proc_t proc_find(proc_id_t pid)
{
	proc_lock(&g_proc_lock);

	proc_t proc = proc_find_live_locked(pid);

	if (
		proc != 0 &&
		!proc_reference_locked(proc)
	) {
		proc = 0;
	}

	proc_unlock(&g_proc_lock);
	return proc;
}

proc_t proc_find_zombie(proc_id_t pid)
{
	proc_lock(&g_proc_lock);

	proc_t proc = proc_find_zombie_locked(pid);

	if (
		proc != 0 &&
		!proc_reference_locked(proc)
	) {
		proc = 0;
	}

	proc_unlock(&g_proc_lock);
	return proc;
}

proc_t proc_find_ident(const proc_ident_t *ident)
{
	if (ident == 0) return 0;

	proc_lock(&g_proc_lock);

	proc_t proc = proc_find_live_locked(ident->pid);

	if (proc == 0) {
		proc =
			proc_find_zombie_locked(
				ident->pid
			);
	}

	if (
		proc == 0 ||
		proc->p_ident.uniqueid !=
			ident->uniqueid ||
		proc->p_ident.idversion !=
			ident->idversion ||
		!proc_reference_locked(proc)
	) {
		proc = 0;
	}

	proc_unlock(&g_proc_lock);
	return proc;
}

bool proc_reference(proc_t proc)
{
	if (proc == 0) return false;

	proc_lock(&g_proc_lock);

	bool result = proc_reference_locked(proc);

	proc_unlock(&g_proc_lock);
	return result;
}

void proc_rele(proc_t proc)
{
	if (proc == 0) return;

	proc_lock(&g_proc_lock);

	if (proc->p_refcount != 0U) {
		proc->p_refcount--;
	}

	if (
		proc->p_refcount == 0U &&
		proc->p_stat == PROC_STATE_DEAD
	) {
		proc_release_slot_locked(proc);
	}

	proc_unlock(&g_proc_lock);
}

bool proc_make_runnable(proc_t proc)
{
	if (proc == 0) return false;

	proc_lock(&g_proc_lock);

	bool valid =
		proc->p_stat == PROC_STATE_EMBRYO &&
		task_is_active(&proc->p_task);

	if (valid) {
		proc->p_stat = PROC_STATE_RUNNABLE;
	}

	proc_unlock(&g_proc_lock);
	return valid;
}

bool proc_mark_running(proc_t proc)
{
	if (proc == 0) return false;

	proc_lock(&g_proc_lock);

	bool valid = proc->p_stat == PROC_STATE_RUNNABLE;

	if (valid) {
		proc->p_stat = PROC_STATE_RUNNING;
	}

	proc_unlock(&g_proc_lock);
	return valid;
}

bool proc_stop(proc_t proc)
{
	if (proc == 0) return false;

	proc_lock(&g_proc_lock);

	bool valid =
		proc->p_stat == PROC_STATE_RUNNING ||
		proc->p_stat == PROC_STATE_RUNNABLE;

	if (valid) {
		proc->p_stat = PROC_STATE_STOPPED;
	}

	proc_unlock(&g_proc_lock);
	return valid;
}

bool proc_continue(proc_t proc)
{
	if (proc == 0) return false;

	proc_lock(&g_proc_lock);

	bool valid = proc->p_stat == PROC_STATE_STOPPED;

	if (valid) {
		proc->p_stat = PROC_STATE_RUNNABLE;
	}

	proc_unlock(&g_proc_lock);
	return valid;
}

bool proc_exit(proc_t proc, uint64_t status)
{
	if (proc == 0) return false;

	proc_lock(&g_proc_lock);

	if (
		proc == g_kernproc ||
		proc->p_stat == PROC_STATE_ZOMBIE ||
		proc->p_stat == PROC_STATE_DEAD ||
		proc->p_stat == PROC_STATE_EMBRYO
	) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	proc->p_flag |= PROC_FLAG_EXITING;
	proc->p_xstat = status;

	if (!task_terminate(&proc->p_task)) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	/* Close descriptor-owned file references before the proc becomes a zombie. */
	filedesc_close_all(&proc->p_fd);

	/* Close owned NXPC name-table references before the proc becomes a zombie. */
	ipc_space_close_all(&proc->p_ipc);

	proc_reparent_children_locked(proc);

	proc_all_remove_locked(proc);

	if (g_proc_count != 0U) {
		g_proc_count--;
	}

	proc->p_stat = PROC_STATE_ZOMBIE;

	proc_zombie_insert_locked(proc);
	g_zombie_count++;

	/* Let a parent that installed a SIGCHLD handler know. */
	signal_notify_parent_of_exit_locked(proc->p_pptr);

	proc_t parent = proc->p_pptr;

	proc_unlock(&g_proc_lock);

	/* A parent sleeping in wait() has something to look at now. Woken
	 * outside the process lock: waking takes the scheduler's. */
	if (parent != 0) waitq_wake_all(&parent->p_waitq);

	return true;
}

bool proc_exit_current(uint64_t status)
{
	proc_t proc = current_proc();

	if (
		proc == 0 ||
		proc == g_kernproc
	) {
		return false;
	}

	return proc_exit(
		proc,
		status
	);
}

proc_wait_result_t proc_wait_child(
	proc_t parent,
	proc_id_t pid,
	proc_id_t *reaped_pid,
	uint64_t *status
)
{
	if (parent == 0 || reaped_pid == 0) return PROC_WAIT_NO_CHILD;

	bool any = pid == PROC_PID_INVALID;
	bool has_child = false;
	proc_t zombie = 0;

	proc_lock(&g_proc_lock);

	if (any) {
		has_child = parent->p_children != 0;

		for (proc_t candidate = g_zombproc_head; candidate != 0; candidate = candidate->p_list_next) {
			if (candidate->p_pptr == parent) {
				zombie = candidate;
				break;
			}
		}
	} else {
		proc_t live = proc_find_live_locked(pid);
		proc_t dead = proc_find_zombie_locked(pid);
		proc_t target = live != 0 ? live : dead;

		has_child = target != 0 && target->p_pptr == parent;
		zombie = (dead != 0 && dead->p_pptr == parent) ? dead : 0;
	}

	proc_unlock(&g_proc_lock);

	if (!has_child) return PROC_WAIT_NO_CHILD;
	if (zombie == 0) return PROC_WAIT_NOT_YET;

	proc_id_t zombie_pid = zombie->p_ident.pid;

	if (!proc_reap(parent, zombie_pid, status)) return PROC_WAIT_NOT_YET;

	*reaped_pid = zombie_pid;
	return PROC_WAIT_REAPED;
}

bool proc_reap(
	proc_t parent,
	proc_id_t pid,
	uint64_t *status
)
{
	if (parent == 0) return false;

	proc_lock(&g_proc_lock);

	proc_t proc = proc_find_zombie_locked(pid);

	if (
		proc == 0 ||
		proc->p_pptr != parent ||
		proc == g_current_proc
	) {
		proc_unlock(&g_proc_lock);
		return false;
	}

	if (status != 0) {
		*status = proc->p_xstat;
	}

	proc_child_remove_locked(proc);
	proc_zombie_remove_locked(proc);

	if (g_zombie_count != 0U) {
		g_zombie_count--;
	}

	proc->p_flag |= PROC_FLAG_REAPED;
	proc->p_stat = PROC_STATE_DEAD;

	if (g_initproc == proc) {
		g_initproc = 0;
	}

	/* Drop the process manager's residency reference. */
	if (proc->p_refcount != 0U) {
		proc->p_refcount--;
	}

	if (proc->p_refcount == 0U) {
		proc_release_slot_locked(proc);
	}

	proc_unlock(&g_proc_lock);
	return true;
}

bool proc_set_current(proc_t proc)
{
	if (proc == 0) return false;

	proc_lock(&g_proc_lock);

	bool valid = proc->p_stat == PROC_STATE_RUNNING;

	if (valid) {
		__atomic_store_n(
			&g_current_proc,
			proc,
			__ATOMIC_RELEASE
		);
	}

	proc_unlock(&g_proc_lock);
	return valid;
}

proc_t current_proc(void)
{
	return __atomic_load_n(
		&g_current_proc,
		__ATOMIC_ACQUIRE
	);
}

proc_id_t proc_selfpid(void)
{
	proc_t proc = current_proc();

	if (proc == 0) {
		return PROC_PID_INVALID;
	}

	return proc->p_ident.pid;
}

proc_id_t proc_ppid(proc_t proc)
{
	if (
		proc == 0 ||
		proc->p_pptr == 0
	) {
		return PROC_PID_INVALID;
	}

	return proc->p_pptr->p_ident.pid;
}

proc_t proc_parent_ref(proc_t proc)
{
	if (proc == 0) return 0;

	proc_lock(&g_proc_lock);

	proc_t parent = proc->p_pptr;

	if (
		parent != 0 &&
		!proc_reference_locked(parent)
	) {
		parent = 0;
	}

	proc_unlock(&g_proc_lock);
	return parent;
}

proc_t proc_first_child_ref(proc_t proc)
{
	if (proc == 0) return 0;

	proc_lock(&g_proc_lock);

	proc_t child = proc->p_children;

	if (
		child != 0 &&
		!proc_reference_locked(child)
	) {
		child = 0;
	}

	proc_unlock(&g_proc_lock);
	return child;
}

proc_t proc_next_sibling_ref(proc_t proc)
{
	if (proc == 0) return 0;

	proc_lock(&g_proc_lock);

	proc_t sibling = proc->p_sibling_next;

	if (
		sibling != 0 &&
		!proc_reference_locked(sibling)
	) {
		sibling = 0;
	}

	proc_unlock(&g_proc_lock);
	return sibling;
}

bool proc_is_inferior(
	proc_t proc,
	proc_t ancestor
)
{
	if (
		proc == 0 ||
		ancestor == 0
	) {
		return false;
	}

	proc_lock(&g_proc_lock);

	proc_t current = proc->p_pptr;
	bool result = false;

	while (current != 0) {
		if (current == ancestor) {
			result = true;
			break;
		}

		current = current->p_pptr;
	}

	proc_unlock(&g_proc_lock);
	return result;
}

task_t proc_task(proc_t proc)
{
	if (proc == 0) return 0;
	return &proc->p_task;
}

vm_address_space_t *proc_vm_map(proc_t proc)
{
	if (proc == 0) return 0;
	return task_map(&proc->p_task);
}

bool proc_get_ident(
	proc_t proc,
	proc_ident_t *ident
)
{
	if (
		proc == 0 ||
		ident == 0
	) {
		return false;
	}

	proc_lock(&g_proc_lock);

	*ident = proc->p_ident;

	proc_unlock(&g_proc_lock);
	return true;
}

bool proc_set_name(
	proc_t proc,
	const char *name
)
{
	if (
		proc == 0 ||
		name == 0
	) {
		return false;
	}

	proc_lock(&g_proc_lock);

	proc_copy_name(
		proc->p_comm,
		name
	);

	proc_unlock(&g_proc_lock);
	return true;
}

bool proc_get_name(
	proc_t proc,
	char *buffer,
	uint32_t size
)
{
	if (
		proc == 0 ||
		buffer == 0 ||
		size == 0U
	) {
		return false;
	}

	proc_lock(&g_proc_lock);

	uint32_t index = 0U;

	while (
		index < size - 1U &&
		index < PROC_NAME_MAX - 1U &&
		proc->p_comm[index] != '\0'
	) {
		buffer[index] = proc->p_comm[index];

		index++;
	}

	buffer[index] = '\0';

	proc_unlock(&g_proc_lock);
	return true;
}

proc_state_t proc_state(proc_t proc)
{
	if (proc == 0) {
		return PROC_STATE_UNUSED;
	}

	return proc->p_stat;
}

static __attribute__((noinline, optnone))
void proc_print_state(proc_state_t state)
{
	if (state == PROC_STATE_UNUSED) {
		kputs("unused");
		return;
	}

	if (state == PROC_STATE_EMBRYO) {
		kputs("embryo");
		return;
	}

	if (state == PROC_STATE_RUNNABLE) {
		kputs("runnable");
		return;
	}

	if (state == PROC_STATE_RUNNING) {
		kputs("running");
		return;
	}

	if (state == PROC_STATE_STOPPED) {
		kputs("stopped");
		return;
	}

	if (state == PROC_STATE_ZOMBIE) {
		kputs("zombie");
		return;
	}

	if (state == PROC_STATE_DEAD) {
		kputs("dead");
		return;
	}

	kputs("unknown");
}

uint32_t proc_child_count(proc_t proc)
{
	if (proc == 0) return 0U;
	return proc->p_childrencnt;
}

uint32_t proc_count(void)
{
	proc_lock(&g_proc_lock);

	uint32_t count = g_proc_count;

	proc_unlock(&g_proc_lock);
	return count;
}

uint32_t proc_zombie_count(void)
{
	proc_lock(&g_proc_lock);

	uint32_t count = g_zombie_count;

	proc_unlock(&g_proc_lock);
	return count;
}

bool proc_validate(void)
{
	proc_lock(&g_proc_lock);

	uint32_t live_count = 0U;
	uint32_t zombie_count = 0U;

	for (
		proc_t proc = g_allproc_head;
		proc != 0;
		proc = proc->p_list_next
	) {
		if (
			proc->p_stat == PROC_STATE_ZOMBIE ||
			proc->p_stat == PROC_STATE_DEAD ||
			task_get_proc(&proc->p_task) != proc
		) {
			proc_unlock(&g_proc_lock);
			return false;
		}

		uint32_t child_count = 0U;

		for (
			proc_t child = proc->p_children;
			child != 0;
			child = child->p_sibling_next
		) {
			if (child->p_pptr != proc) {
				proc_unlock(&g_proc_lock);
				return false;
			}

			child_count++;
		}

		if (
			child_count !=
			proc->p_childrencnt
		) {
			proc_unlock(&g_proc_lock);
			return false;
		}

		live_count++;
	}

	for (
		proc_t proc = g_zombproc_head;
		proc != 0;
		proc = proc->p_list_next
	) {
		if (
			proc->p_stat != PROC_STATE_ZOMBIE ||
			task_is_active(&proc->p_task)
		) {
			proc_unlock(&g_proc_lock);
			return false;
		}

		zombie_count++;
	}

	bool valid =
		live_count == g_proc_count &&
		zombie_count == g_zombie_count &&
		g_kernproc != 0 &&
		g_kernproc->p_ident.pid ==
			PROC_PID_KERNEL &&
		g_current_proc != 0;

	proc_unlock(&g_proc_lock);
	return valid;
}

void proc_dump(void)
{
	proc_lock(&g_proc_lock);

	kputln("NXU_ProcessManager: live process list");

	for (
		proc_t proc = g_allproc_head;
		proc != 0;
		proc = proc->p_list_next
	) {
		kputs("NXU_ProcessManager: pid ");
		kputu64(proc->p_ident.pid);

		kputs(", ppid ");

		if (proc->p_pptr != 0) {
			kputu64(
				proc->p_pptr->p_ident.pid
			);
		} else {
			kputs("-");
		}

		kputs(", state ");
		proc_print_state(proc->p_stat);
		kputs(", refs ");
		kputu64(proc->p_refcount);

		kputs(", name ");
		kputln(proc->p_comm);
	}

	if (g_zombproc_head != 0) {
		kputln("NXU_ProcessManager: zombie process list");
	}

	for (
		proc_t proc = g_zombproc_head;
		proc != 0;
		proc = proc->p_list_next
	) {
		kputs("NXU_ProcessManager: pid ");
		kputu64(proc->p_ident.pid);

		kputs(", state zombie, status ");
		kputu64(
			(uint64_t)proc->p_xstat
		);

		kputs(", name ");
		kputln(proc->p_comm);
	}

	proc_unlock(&g_proc_lock);
}
