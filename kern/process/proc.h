/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/proc.h
 *
 * Declares the kernel process proc interfaces and data structures.
 */
#ifndef NXU_KERN_PROC_H
#define NXU_KERN_PROC_H

#include <kern/ipc/ipc_space.h>
#include <kern/sched_prism/waitq.h>
#include <kern/security/ucred.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/process/task.h>
#include <vfs/file.h>

#include <stdbool.h>
#include <stdint.h>

#define PROC_MAX 256U /* storage ceiling; the runtime limit is resource-derived */
#define PROC_PID_KERNEL 0U
#define PROC_PID_MAX 99999U
#define PROC_PID_INVALID UINT32_MAX

#define PROC_NAME_MAX 32U

/* Longest working directory, with its terminator: VFS_PATH_MAX. */
#define PROC_CWD_MAX 256U

/* proc::p_jobevent: a change of state the parent has not collected with wait yet. */
#define PROC_JOBEVENT_STOPPED (1U << 0U)
#define PROC_JOBEVENT_CONTINUED (1U << 1U)

typedef uint32_t proc_id_t;
typedef uint64_t proc_uniqueid_t;
typedef uint32_t proc_idversion_t;

struct pgrp;

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
	PROC_FLAG_TRACED = 1U << 4U,
	PROC_FLAG_EXECED = 1U << 5U /* has replaced its image with exec at least once */
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
 * p_pgrp / p_pglist_next / p_pglist_prev
 *     The process group this process belongs to, and its links on that
 *     group's member list. Set before the process is visible and kept until
 *     it is reaped. Protected by the process table lock; see
 *     kern/process/pgrp.h.
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
 * p_stopq / p_stop_signal / p_jobevent / p_cont_seq
 *     Job control. While the process is stopped (p_stat is
 *     PROC_STATE_STOPPED) its threads sleep on p_stopq at their return to
 *     user mode, and p_stop_signal is the signal that stopped it. p_jobevent
 *     is what its parent has not yet been told through wait: that it stopped,
 *     or that it was continued (PROC_JOBEVENT_*). The three are protected by
 *     the process table lock. p_cont_seq counts SIGCONTs and is protected by
 *     p_siglock. See kern/process/signal.h.
 *
 * p_sigact / p_sigpending
 *     Signal dispositions and the set of signals sent but not yet delivered
 *     (bit n = signal n). All threads of a process share both; the blocked
 *     mask is per thread. See kern/process/signal.h.
 *
 * p_cwd / p_cwd_lock
 *     The working directory, which relative paths are resolved against: a
 *     canonical absolute path (vfs/pathname.h), not a vnode, because the VFS
 *     looks paths up by name from the root. Inherited from the parent; "/"
 *     for the kernel and PID 1. Only the process changes its own (chdir);
 *     its threads read it for every path they give, and a child being made
 *     copies it. p_cwd_lock, taken alone and briefly, covers it.
 *
 * p_caps
 *     What this process may do to the system beyond computing and IPC: a mask
 *     of NXU_CAP_* (kern/syscall/syscall_defs.h). Default-deny: none, unless
 *     it is PID 1 (all of them), its parent granted a subset when spawning it,
 *     or the kernel set them on a test process (loader_spawn_caps, before its thread can run
 *     on any CPU; proc_set_caps after the fact would race). fork and exec
 *     keep the caller's mask. It remains the machine-mutation gate; Unix
 *     identity now lives alongside it in p_ucred, bridged so that effective
 *     UID 0 (root) is treated as holding every capability. See
 *     doc/architecture/unix-security.md.
 *
 * p_ucred
 *     Unix user/group credentials (real/effective/saved UID and GID plus
 *     supplementary groups). The kernel task and PID 1 start as root;
 *     proc_create_user and proc_fork inherit the parent's credentials; exec
 *     preserves them. See kern/security/ucred.h.
 */
struct proc {
	uint64_t p_gui_epoch, p_gui_features;
	uint32_t p_gui_record_limit;
	uint16_t p_gui_minor;

	char p_dyrld_main[NXU_DYRLD_PATH];
	uint32_t p_dyrld_count;
	bool p_dyrld_finalized;
	uint64_t p_dyrld_relro_start[NXU_DYRLD_IMAGES], p_dyrld_relro_end[NXU_DYRLD_IMAGES];
	nxu_dyrld_image_t p_dyrld_interpreter;
	nxu_dyrld_image_t p_dyrld_images[NXU_DYRLD_IMAGES];
	proc_t p_list_prev;
	proc_t p_list_next;

	proc_t p_pptr;
	proc_t p_children;

	proc_t p_sibling_prev;
	proc_t p_sibling_next;

	struct pgrp *p_pgrp;
	proc_t p_pglist_next;
	proc_t p_pglist_prev;

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
	waitq_t p_stopq;
	uint32_t p_stop_signal;
	uint32_t p_jobevent;
	uint32_t p_cont_seq;
	uint32_t p_sigpending;
	uint32_t p_caps;

	nxu_spinlock_t p_cwd_lock;
	char p_cwd[PROC_CWD_MAX];

	struct ucred p_ucred;

	proc_ident_t p_ident;

	proc_state_t p_stat;
	uint32_t p_flag;

	uint32_t p_refcount;
	uint32_t p_childrencnt;

	uint64_t p_xstat;

	char p_comm[PROC_NAME_MAX];
	char p_exec_path[NXU_CRASH_PATH_MAX];
	uint8_t p_image_id[32];
	nxu_crash_report_t *p_crash_report;

	uint32_t p_slot;
};

bool proc_bootstrap(void);
uint32_t proc_limit(void);

proc_t proc_kernel(void);
proc_t proc_initproc(void);

bool proc_create_user(proc_t parent, const char *name, uint64_t entry, uint64_t stack, proc_t *result);

/*
 * proc_group_t
 *
 * Where a process that is being created goes among process groups and
 * sessions (kern/process/pgrp.h). The placement is part of creating it: the
 * process is in its group in the same critical section that makes it visible,
 * so there is never a moment at which it can be found, signalled or run in
 * the wrong group, and its creator never has to move it afterwards.
 */
typedef enum {
	PROC_GROUP_INHERIT,	   /* the parent's group */
	PROC_GROUP_NEW,		   /* a new group in the parent's session, named after the new process */
	PROC_GROUP_JOIN,	   /* the existing group `pgid` of the parent's session */
	PROC_GROUP_NEW_SESSION /* a new session and group, both named after the new process */
} proc_group_mode_t;

typedef struct {
	proc_group_mode_t mode;
	proc_id_t pgid;
} proc_group_t;

typedef enum { PROC_CREATE_OK, PROC_CREATE_FAILED, PROC_CREATE_GROUP_DENIED } proc_create_result_t;

/*
 * proc_create_user_in
 *
 * proc_create_user with the new process placed as `group` says (0: inherit).
 * PROC_CREATE_GROUP_DENIED means the group to join does not exist or belongs
 * to another session; nothing was created. A process whose parent is the
 * kernel process leads a session of its own whatever `group` says.
 */
proc_create_result_t proc_create_user_in(proc_t parent, const char *name, uint64_t entry, uint64_t stack,
										 const proc_group_t *group, proc_t *result);

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


bool proc_exit(proc_t proc, uint64_t status);

/*
 * proc_abort_embryo
 *
 * Take apart a process that was created but never started (it is still
 * PROC_STATE_EMBRYO): its unstarted thread and address space, its
 * descriptors and ports, its place in its group and under its parent, and
 * its slot. Nothing remains to wait for: an embryo has no exit status, and
 * to its parent it never existed. False if proc is not an embryo. This is how
 * a spawn that fails part-way leaves no half-made process behind.
 */
bool proc_abort_embryo(proc_t proc);
bool proc_exit_current(uint64_t status);

typedef enum { PROC_WAIT_REAPED, PROC_WAIT_CHANGED, PROC_WAIT_NOT_YET, PROC_WAIT_NO_CHILD } proc_wait_result_t;

/*
 * proc_wait_child
 *
 * One non-blocking pass of wait(): reap a zombie child of parent -- the one
 * named by pid, or any of them for PROC_PID_INVALID -- reporting its pid and
 * exit status (PROC_WAIT_REAPED). With NXU_WAIT_UNTRACED or
 * NXU_WAIT_CONTINUED_OPTION in options, a child that has stopped or been
 * continued since it was last reported is reported instead, once, with a
 * NXU_WAIT_STOPPED or NXU_WAIT_CONTINUED status, and is not reaped
 * (PROC_WAIT_CHANGED). An exit is preferred over either. PROC_WAIT_NOT_YET
 * means there is nothing to report yet; PROC_WAIT_NO_CHILD means there is
 * nothing to wait for.
 */
proc_wait_result_t proc_wait_child(proc_t parent, proc_id_t pid, uint32_t options, proc_id_t *reaped_pid,
								   uint64_t *status);

bool proc_reap(proc_t parent, proc_id_t pid, uint64_t *status);

/* Record that proc has replaced its image with exec (PROC_FLAG_EXECED). */
void proc_mark_execed(proc_t proc);

bool proc_set_current(proc_t proc);
proc_t current_proc(void);

proc_id_t proc_selfpid(void);
proc_id_t proc_ppid(proc_t proc);

/* True when proc holds every capability in caps (NXU_CAP_*). */
bool proc_has_caps(proc_t proc, uint32_t caps);

/*
 * proc_get_cwd / proc_set_cwd
 *
 * Copy proc's working directory out (PROC_CWD_MAX bytes), and replace it
 * with `path`, which the caller has made canonical and checked is a
 * directory it may search. proc_set_cwd is false for a path too long.
 */
void proc_get_cwd(proc_t proc, char cwd[PROC_CWD_MAX]);
bool proc_set_cwd(proc_t proc, const char *path);

/* Replace proc's capability mask. Only for the kernel setting up a process
 * that has not run yet (a test spawn) and for the spawn system call, which
 * checks the mask against the parent's first. */
void proc_set_caps(proc_t proc, uint32_t caps);

/* The process's Unix credentials (never null for a live process). */
struct ucred *proc_ucred(proc_t proc);

/* True when proc's effective UID is root (NXU_UID_ROOT). Drives the
 * credential->capability bridge in proc_has_caps. */
bool proc_is_root(proc_t proc);

proc_t proc_parent_ref(proc_t proc);
proc_t proc_first_child_ref(proc_t proc);
proc_t proc_next_sibling_ref(proc_t proc);

bool proc_is_inferior(proc_t proc, proc_t ancestor);

task_t proc_task(proc_t proc);
vm_address_space_t *proc_vm_map(proc_t proc);

bool proc_get_ident(proc_t proc, proc_ident_t *ident);

bool proc_set_name(proc_t proc, const char *name);

bool proc_get_name(proc_t proc, char *buffer, uint32_t size);

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

#include <kern/syscall/statistics.h>
/* Binds to the boot CPU to serialize map replacement/destruction by exec/exit;
 * map locks still serialize remote faults and mmap/munmap. Returns total live
 * processes and fills at most capacity records. */
uint32_t proc_memory_snapshot(nxu_stats_process_memory_t *out, uint32_t capacity);

bool proc_validate(void);
void proc_dump(void);

#endif
