#ifndef NXU_KERN_PROC_H
#define NXU_KERN_PROC_H

#include <kern/ipc/ipc_space.h>
#include <kern/sched_prism/waitq.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/process/task.h>
#include <vfs/file.h>

#include <stdbool.h>
#include <stdint.h>

#define PROC_MAX 64U
#define PROC_PID_KERNEL 0U
#define PROC_PID_MAX 99999U
#define PROC_PID_INVALID UINT32_MAX

#define PROC_NAME_MAX 32U

typedef uint32_t proc_id_t;
typedef uint64_t proc_uniqueid_t;
typedef uint32_t proc_idversion_t;

typedef struct proc *proc_t;

typedef enum {
	PROC_STATE_UNUSED,
	PROC_STATE_EMBRYO,
	PROC_STATE_RUNNABLE,
	PROC_STATE_RUNNING,
	PROC_STATE_STOPPED,
	PROC_STATE_ZOMBIE,
	PROC_STATE_DEAD
} proc_state_t;

typedef enum {
	PROC_FLAG_NONE = 0U,
	PROC_FLAG_SYSTEM = 1U << 0U,
	PROC_FLAG_EXITING = 1U << 1U,
	PROC_FLAG_REAPED = 1U << 2U,
	PROC_FLAG_EXECING = 1U << 3U,
	PROC_FLAG_TRACED = 1U << 4U
} proc_flag_t;

/*
 * proc_ident_t
 *
 * Stable identity for one process lifetime.
 *
 * A PID may be reused. uniqueid and idversion distinguish a stale identity
 * from a later process which happens to receive the same PID.
 */
typedef struct {
	proc_id_t pid;
	proc_uniqueid_t uniqueid;
	proc_idversion_t idversion;
} proc_ident_t;

/*
 * struct proc
 *
 * UNIX process identity and lifecycle state.
 *
 * Execution resources belong to p_task. Open-file state belongs to p_fd and
 * is shared by every thread in the task. Live processes reside on allproc;
 * exited processes reside on zombproc until their parent reaps them.
 *
 * p_pptr / p_children / p_sibling_*
 *     Intrusive process hierarchy.
 *
 * p_task
 *     Address space and thread container.
 *
 * p_fd
 *     Per-process descriptor table. Descriptor-owned file references are
 *     closed before the process enters zombie state.
 *
 * p_ipc
 *     Per-process NXPC name table. Owned port references are closed before
 *     the process enters zombie state, exactly like p_fd.
 *
 * p_ipc_bootstrap_name
 *     Name, within p_ipc, of the send right to the system bootstrap
 *     registry inherited at spawn (see loader_spawn). IPC_SPACE_NAME_INVALID
 *     until spawn-time handoff is wired up.
 *
 * p_waitq
 *     Threads sleeping in wait() for one of this process's children to exit.
 *     proc_exit wakes it.
 *
 * p_sigact / p_sigpending
 *     Signal dispositions and the set of signals sent but not yet delivered
 *     (bit n = signal n). All threads of a process share both; the blocked
 *     mask is per thread. See kern/process/signal.h.
 *
 * p_caps
 *     What this process may do to the system beyond computing and IPC: a mask
 *     of NXU_CAP_* (kern/syscall/syscall_defs.h). Default-deny: none, unless
 *     it is PID 1 (all of them), its parent granted a subset when spawning it,
 *     or the kernel set them on a test process (loader_spawn_caps, before its thread can run
 *     on any CPU; proc_set_caps after the fact would race). fork and exec
 *     keep the caller's mask. There are no credentials; this mask is the whole
 *     security model for the system calls that change the machine.
 */
struct proc {
	proc_t p_list_prev;
	proc_t p_list_next;

	proc_t p_pptr;
	proc_t p_children;

	proc_t p_sibling_prev;
	proc_t p_sibling_next;

	struct task p_task;
	struct filedesc p_fd;
	struct ipc_space p_ipc;
	uint32_t p_ipc_bootstrap_name;

	/*
	 * p_siglock protects p_sigact and p_sigpending (threads of this process on
	 * different CPUs send, catch and dequeue signals at the same time). It is a
	 * leaf: taken with interrupts masked, nothing is called while it is held,
	 * and it may be taken under the process table lock.
	 */
	nxu_spinlock_t p_siglock;
	nxu_sigaction_t p_sigact[NXU_NSIG];
	waitq_t p_waitq;
	uint32_t p_sigpending;
	uint32_t p_caps;

	proc_ident_t p_ident;

	proc_state_t p_stat;
	uint32_t p_flag;

	uint32_t p_refcount;
	uint32_t p_childrencnt;

	uint64_t p_xstat;

	char p_comm[PROC_NAME_MAX];

	uint32_t p_slot;
};

bool proc_bootstrap(void);

proc_t proc_kernel(void);
proc_t proc_initproc(void);

bool proc_create_user(
	proc_t parent,
	const char *name,
	uint64_t entry,
	uint64_t stack,
	proc_t *result
);

/*
 * proc_fork
 *
 * Create a child of parent that is a copy of it, as the fork system call
 * does: a copy-on-write copy of the address space and region list, shared
 * open files and port names, copied signal dispositions and the calling
 * thread's blocked mask. The child's first thread resumes at the point
 * fork() returns in the parent, with 0 as the result. Must be called by
 * parent's own thread while it is inside the kernel for a system call.
 * The child is left runnable and already scheduled.
 */
bool proc_fork(proc_t parent, proc_t *child);

proc_t proc_find(proc_id_t pid);
proc_t proc_find_zombie(proc_id_t pid);
proc_t proc_find_ident(const proc_ident_t *ident);

bool proc_reference(proc_t proc);
void proc_rele(proc_t proc);

bool proc_make_runnable(proc_t proc);
bool proc_mark_running(proc_t proc);

bool proc_stop(proc_t proc);
bool proc_continue(proc_t proc);

bool proc_exit(proc_t proc, uint64_t status);
bool proc_exit_current(uint64_t status);

typedef enum {
	PROC_WAIT_REAPED,
	PROC_WAIT_NOT_YET,
	PROC_WAIT_NO_CHILD
} proc_wait_result_t;

/*
 * proc_wait_child
 *
 * One non-blocking pass of wait(): reap a zombie child of parent -- the one
 * named by pid, or any of them for PROC_PID_INVALID -- reporting its pid and
 * exit status. PROC_WAIT_NOT_YET means the child (or every child) is still
 * running; PROC_WAIT_NO_CHILD means there is nothing to wait for.
 */
proc_wait_result_t proc_wait_child(
	proc_t parent,
	proc_id_t pid,
	proc_id_t *reaped_pid,
	uint64_t *status
);

bool proc_reap(
	proc_t parent,
	proc_id_t pid,
	uint64_t *status
);

bool proc_set_current(proc_t proc);
proc_t current_proc(void);

proc_id_t proc_selfpid(void);
proc_id_t proc_ppid(proc_t proc);

/* True when proc holds every capability in caps (NXU_CAP_*). */
bool proc_has_caps(proc_t proc, uint32_t caps);

/* Replace proc's capability mask. Only for the kernel setting up a process
 * that has not run yet (a test spawn) and for the spawn system call, which
 * checks the mask against the parent's first. */
void proc_set_caps(proc_t proc, uint32_t caps);

proc_t proc_parent_ref(proc_t proc);
proc_t proc_first_child_ref(proc_t proc);
proc_t proc_next_sibling_ref(proc_t proc);

bool proc_is_inferior(
	proc_t proc,
	proc_t ancestor
);

task_t proc_task(proc_t proc);
vm_address_space_t *proc_vm_map(proc_t proc);

bool proc_get_ident(
	proc_t proc,
	proc_ident_t *ident
);

bool proc_set_name(
	proc_t proc,
	const char *name
);

bool proc_get_name(
	proc_t proc,
	char *buffer,
	uint32_t size
);

proc_state_t proc_state(proc_t proc);

uint32_t proc_child_count(proc_t proc);
uint32_t proc_count(void);
uint32_t proc_zombie_count(void);

/*
 * proc_snapshot
 *
 * Copy what Activity Monitor shows of every live process (zombies are left
 * out) into `out`, at most `capacity` of them, and return how many were
 * copied. Only the process table lock is taken; CPU time comes from
 * thread_snapshot(), joined on uniqueid.
 */
typedef struct {
	proc_uniqueid_t uniqueid;
	proc_id_t pid;
	proc_id_t ppid;
	proc_state_t state;
	uint32_t flags;
	uint32_t thread_count;
	char name[PROC_NAME_MAX];
} proc_snapshot_t;

uint32_t proc_snapshot(proc_snapshot_t *out, uint32_t capacity);

bool proc_validate(void);
void proc_dump(void);

#endif
