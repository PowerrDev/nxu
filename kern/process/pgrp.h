/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/pgrp.h
 *
 * Process groups and sessions: the two levels of grouping above the process
 * that job control is built on.
 *
 * Model:
 *
 *   - Every process belongs to exactly one process group, and every process
 *     group to exactly one session. A group is named by a process ID (the
 *     pid of the process that created it, its leader), and so is a session.
 *     A group or session outlives its leader for as long as it has members.
 *
 *   - A child starts in its parent's group, and therefore its session. This
 *     holds for fork and for spawn alike, and the child is in the group
 *     before it can run or be found by pid. The one exception is a process
 *     the kernel itself starts (its parent is the kernel process): it has no
 *     user-level ancestor to share a session with, so it starts as the leader
 *     of a new session and a new group. PID 1 is the usual case.
 *
 *   - A process changes group with setpgid and session with setsid, under the
 *     POSIX rules stated at each call below. Nothing else moves a process.
 *
 *   - A process stays a member of its group after it exits, until its parent
 *     reaps it: a zombie can still be waited for by group. A pid is not
 *     reused while any group or session is still named by it.
 *
 * This file owns the relationships and nothing more. Which terminal a
 * session controls, and which of its groups is in the foreground, is
 * terminal state and lives with the terminal; it refers to groups and
 * sessions by ID.
 *
 * Ownership and locking: groups and sessions are heap objects created by
 * setpgid, setsid and process creation, and freed when their last member
 * leaves (a session, when its last group goes). All of it, and the p_pgrp and
 * p_pglist_* fields of struct proc, is protected by the process table lock
 * (kern/process/proc_internal.h). Memory is allocated before that lock is
 * taken and freed after it is dropped.
 */

#ifndef NXU_KERN_PROCESS_PGRP_H
#define NXU_KERN_PROCESS_PGRP_H

#include <kern/process/proc.h>

#include <stdbool.h>
#include <stdint.h>

struct pgrp;
struct session;

typedef enum {
	PGRP_STATUS_OK,
	PGRP_STATUS_NO_SUCH_PROCESS,
	PGRP_STATUS_DENIED,
	PGRP_STATUS_INVALID,
	PGRP_STATUS_NO_MEMORY
} pgrp_status_t;

/*
 * proc_getpgid / proc_getsid
 *
 * The process group or session ID of the live process `pid` (0: the caller).
 */
pgrp_status_t proc_getpgid(proc_t caller, proc_id_t pid, proc_id_t *pgid);
pgrp_status_t proc_getsid(proc_t caller, proc_id_t pid, proc_id_t *sid);

/*
 * proc_setpgid
 *
 * Move process `pid` (0: the caller) into group `pgid` (0: the group named
 * after the process itself), creating that group if the process is naming a
 * group after itself. The rules:
 *
 *   - the process is the caller or one of its children
 *     (else NO_SUCH_PROCESS);
 *   - a child that has already replaced its image with exec may not be moved
 *     (DENIED): by then it may be relying on the group it is in;
 *   - the process is in the caller's session and is not a session leader
 *     (DENIED);
 *   - the group either exists in that same session, or is the one named
 *     after the process (DENIED otherwise).
 */
pgrp_status_t proc_setpgid(proc_t caller, proc_id_t pid, proc_id_t pgid);

/*
 * proc_setsid
 *
 * Make the caller the leader of a new session and of a new group in it,
 * both named after it; *sid is the new session ID. Refused (DENIED) for a
 * process that already leads a group, or whose pid still names a group:
 * otherwise that group would straddle two sessions. A new session has no
 * controlling terminal.
 */
pgrp_status_t proc_setsid(proc_t caller, proc_id_t *sid);

/* True when proc is the leader of its session. */
bool proc_is_session_leader(proc_t proc);

/*
 * pgrp_members_ref
 *
 * The live members of process group pgid, each with a reference the caller
 * must drop (proc_rele), at most `capacity` of them. Returns how many; 0 if
 * there is no such group. This is how a signal reaches a group: the members
 * are collected under the lock and signalled after it is dropped.
 */
uint32_t pgrp_members_ref(proc_id_t pgid, proc_t *members, uint32_t capacity);

/*
 * proc_pgrp_is_orphaned
 *
 * True when proc's process group is orphaned: no member has a parent that
 * is in a different group of the same session. Such a group has nobody in
 * its session who could resume it (the shell that would have is gone, or was
 * never there), so a stop asked for from the terminal is not acted on in it,
 * and when a group becomes orphaned while members are stopped they are sent
 * SIGHUP and SIGCONT (pgrp_orphaned_by_exit_locked).
 */
bool proc_pgrp_is_orphaned(proc_t proc);

/*
 * The halves used by kern/process/proc.c. All run with the process table
 * lock held, and none allocates or frees.
 */

/*
 * What a process left behind when it changed or left its group: to be freed,
 * by pgrp_reclaim, after the lock is dropped. ctty is a controlling terminal
 * its session gave up (its leader left), with the session's reference, which
 * pgrp_reclaim hands back after telling the terminal (tty_session_leader_exit).
 */
typedef struct {
	struct pgrp *pgrp;
	struct session *session;
	struct tty *ctty;
	proc_id_t ctty_sid;
} pgrp_reclaim_t;

/* A group and a session to build with, allocated ahead of the lock. Either may be 0 on return. */
bool pgrp_prepare(struct pgrp **pgrp, struct session **session);
void pgrp_discard(struct pgrp *pgrp, struct session *session);
void pgrp_reclaim(pgrp_reclaim_t *reclaim);

/* A new process joins its parent's group. */
void pgrp_inherit_locked(proc_t child, proc_t parent);

/*
 * pgrp_place_locked
 *
 * Put a process that is being created where `request` says, relative to its
 * parent: in the parent's group (PROC_GROUP_INHERIT), in a new group named
 * after itself (PROC_GROUP_NEW), in the existing group request->pgid of the
 * parent's session (PROC_GROUP_JOIN), or at the head of a new session
 * (PROC_GROUP_NEW_SESSION). *pgrp and *session are prepared objects; the ones
 * used are set to 0. Fails with PGRP_STATUS_DENIED, placing the child
 * nowhere, when the group to join does not exist or is in another session.
 */
pgrp_status_t pgrp_place_locked(proc_t child, proc_t parent, const proc_group_t *request, struct pgrp **pgrp,
								struct session **session);

/*
 * proc becomes the leader of a new session and group, both named after it,
 * built from the prepared objects (which it consumes). Whatever it leaves
 * behind empty is returned in *reclaim.
 */
void pgrp_new_session_locked(proc_t proc, struct pgrp *pgrp, struct session *session, pgrp_reclaim_t *reclaim);

/* A reaped process leaves its group. */
void pgrp_leave_locked(proc_t proc, pgrp_reclaim_t *reclaim);

/*
 * An exiting process stops being its session's leader; the session lives on
 * without one, and without its controlling terminal, which goes in *reclaim.
 */
void pgrp_leader_exit_locked(proc_t proc, pgrp_reclaim_t *reclaim);

/*
 * pgrp_orphaned_by_exit_locked
 *
 * `exiting` is about to stop being anyone's parent or a member of its group.
 * Reports the groups that this orphans and that have a stopped member: its
 * own group and those of its children are the only candidates. Stores their
 * IDs in pgids (at most capacity) and returns how many. The caller sends each
 * SIGHUP and then SIGCONT once the lock is dropped. Must be called before
 * exiting's children are given to another parent.
 */
uint32_t pgrp_orphaned_by_exit_locked(proc_t exiting, proc_id_t *pgids, uint32_t capacity);

/*
 * Controlling terminals. The process subsystem keeps, for each session, a
 * counted reference to its controlling terminal (kern/tty/tty.h), and the
 * terminal keeps the session's and its foreground group's IDs. Both sides
 * change together under the process table lock and then the terminal's lock:
 * the process table lock may be held while a terminal's is taken, never the
 * other way round (kern/tty/tty_jobctl.c).
 *
 * proc_job_get
 *     proc's session and group IDs, whether it leads the session, and the
 *     session's controlling terminal with a reference for the caller
 *     (tty_release), or TTY_NULL. False for a process with no group.
 *
 * proc_ctty_acquire
 *     `tty` becomes the controlling terminal of proc's session, with proc's
 *     group in the foreground. proc must lead the session, the session must
 *     have none, and no other session may have this one (DENIED otherwise).
 *
 * proc_session_drop_tty
 *     Session `sid` no longer has `tty` as its controlling terminal. Returns
 *     the session's reference for the caller to release, or TTY_NULL when the
 *     session did not have it.
 *
 * pgrp_in_session
 *     Whether pgid names a process group of session sid.
 */
struct tty;

typedef struct {
	proc_id_t sid;
	proc_id_t pgid;
	bool leader;
	struct tty *ctty;
} proc_job_t;

bool proc_job_get(proc_t proc, proc_job_t *job);
pgrp_status_t proc_ctty_acquire(proc_t proc, struct tty *tty);
struct tty *proc_session_drop_tty(proc_id_t sid, struct tty *tty);
bool pgrp_in_session(proc_id_t pgid, proc_id_t sid);

/* True while a group or a session is named `id`: such a pid must not be handed out again. */
bool pgrp_id_in_use_locked(proc_id_t id);

#endif
