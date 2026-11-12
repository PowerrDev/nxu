/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/pgrp.c
 *
 * Process groups and sessions. See kern/process/pgrp.h for the model.
 *
 * Reference counting: there is none to get wrong, because membership is the
 * reference. A group exists exactly while pg_count members are linked on
 * pg_members; a session exists exactly while s_count groups point at it.
 * pgrp_detach_locked is the only place either count goes down, and it hands
 * whatever reached zero back to the caller to free once the lock is dropped.
 *
 * Locking: everything here is under the process table lock, taken by the
 * public calls and already held by the *_locked ones. kmalloc and kfree are
 * never called with it held.
 */

#include <kern/process/pgrp.h>

#include <kern/memory/heap.h>
#include <kern/process/proc_internal.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * struct session
 *
 * s_sid
 *     The session's name: the pid of the process that created it.
 *
 * s_leader
 *     That process, while it is alive; 0 once it has exited. Not a counted
 *     reference: it is cleared, under the lock, before the process can go.
 *
 * s_count
 *     Process groups in the session.
 */
struct session {
	proc_id_t s_sid;
	proc_t s_leader;
	uint32_t s_count;
};

/*
 * struct pgrp
 *
 * pg_id
 *     The group's name: the pid of the process that created it.
 *
 * pg_session
 *     The session the group belongs to, for its whole life.
 *
 * pg_members / pg_count
 *     The member processes, linked through p_pglist_next/prev, and how many.
 *
 * pg_next / pg_prev
 *     The list of all groups.
 */
struct pgrp {
	proc_id_t pg_id;
	struct session *pg_session;
	proc_t pg_members;
	uint32_t pg_count;
	struct pgrp *pg_next;
	struct pgrp *pg_prev;
};

static struct pgrp *g_pgrp_head;

bool
pgrp_prepare(struct pgrp **pgrp, struct session **session)
{
	if (pgrp != 0) *pgrp = 0;
	if (session != 0) *session = 0;

	if (pgrp != 0) {
		*pgrp = kcalloc(1U, sizeof(struct pgrp));

		if (*pgrp == 0) return false;
	}

	if (session != 0) {
		*session = kcalloc(1U, sizeof(struct session));

		if (*session == 0) {
			if (pgrp != 0) {
				(void)kfree(*pgrp);
				*pgrp = 0;
			}

			return false;
		}
	}

	return true;
}

void
pgrp_discard(struct pgrp *pgrp, struct session *session)
{
	if (pgrp != 0) (void)kfree(pgrp);
	if (session != 0) (void)kfree(session);
}

void
pgrp_reclaim(pgrp_reclaim_t *reclaim)
{
	if (reclaim == 0) return;

	pgrp_discard(reclaim->pgrp, reclaim->session);
	reclaim->pgrp = 0;
	reclaim->session = 0;
}

static struct pgrp *
pgrp_find_locked(proc_id_t pgid)
{
	for (struct pgrp *pgrp = g_pgrp_head; pgrp != 0; pgrp = pgrp->pg_next) {
		if (pgrp->pg_id == pgid) return pgrp;
	}

	return 0;
}

bool
pgrp_id_in_use_locked(proc_id_t id)
{
	for (struct pgrp *pgrp = g_pgrp_head; pgrp != 0; pgrp = pgrp->pg_next) {
		if (pgrp->pg_id == id || pgrp->pg_session->s_sid == id) return true;
	}

	return false;
}

/* Publish an empty group named `pgid` in `session`. */
static void
pgrp_create_locked(struct pgrp *pgrp, proc_id_t pgid, struct session *session)
{
	pgrp->pg_id = pgid;
	pgrp->pg_session = session;
	pgrp->pg_members = 0;
	pgrp->pg_count = 0U;
	pgrp->pg_prev = 0;
	pgrp->pg_next = g_pgrp_head;

	if (g_pgrp_head != 0) g_pgrp_head->pg_prev = pgrp;

	g_pgrp_head = pgrp;
	session->s_count++;
}

static void
pgrp_attach_locked(proc_t proc, struct pgrp *pgrp)
{
	proc->p_pgrp = pgrp;
	proc->p_pglist_prev = 0;
	proc->p_pglist_next = pgrp->pg_members;

	if (pgrp->pg_members != 0) pgrp->pg_members->p_pglist_prev = proc;

	pgrp->pg_members = proc;
	pgrp->pg_count++;
}

/*
 * pgrp_detach_locked:
 *
 * Take proc out of its group. A group left without members is unpublished,
 * and so is its session if that was its last group; both are returned in
 * *reclaim for the caller to free after the lock is dropped. *reclaim must
 * be empty on entry.
 */
static void
pgrp_detach_locked(proc_t proc, pgrp_reclaim_t *reclaim)
{
	struct pgrp *pgrp = proc->p_pgrp;

	if (pgrp == 0) return;

	if (proc->p_pglist_prev != 0) {
		proc->p_pglist_prev->p_pglist_next = proc->p_pglist_next;
	} else {
		pgrp->pg_members = proc->p_pglist_next;
	}

	if (proc->p_pglist_next != 0) proc->p_pglist_next->p_pglist_prev = proc->p_pglist_prev;

	proc->p_pglist_next = 0;
	proc->p_pglist_prev = 0;
	proc->p_pgrp = 0;
	pgrp->pg_count--;

	if (pgrp->pg_count != 0U) return;

	if (pgrp->pg_prev != 0) {
		pgrp->pg_prev->pg_next = pgrp->pg_next;
	} else {
		g_pgrp_head = pgrp->pg_next;
	}

	if (pgrp->pg_next != 0) pgrp->pg_next->pg_prev = pgrp->pg_prev;

	struct session *session = pgrp->pg_session;

	reclaim->pgrp = pgrp;
	session->s_count--;

	if (session->s_count == 0U) reclaim->session = session;
}

void
pgrp_inherit_locked(proc_t child, proc_t parent)
{
	if (child == 0 || parent == 0 || parent->p_pgrp == 0) return;

	pgrp_attach_locked(child, parent->p_pgrp);
}

pgrp_status_t
pgrp_place_locked(proc_t child, proc_t parent, const proc_group_t *request, struct pgrp **pgrp,
				  struct session **session)
{
	if (child == 0 || parent == 0 || request == 0 || parent->p_pgrp == 0) return PGRP_STATUS_INVALID;

	switch (request->mode) {
	case PROC_GROUP_INHERIT:
		pgrp_attach_locked(child, parent->p_pgrp);
		return PGRP_STATUS_OK;

	case PROC_GROUP_NEW:
		if (pgrp == 0 || *pgrp == 0) return PGRP_STATUS_NO_MEMORY;

		pgrp_create_locked(*pgrp, child->p_ident.pid, parent->p_pgrp->pg_session);
		pgrp_attach_locked(child, *pgrp);
		*pgrp = 0;
		return PGRP_STATUS_OK;

	case PROC_GROUP_JOIN: {
		struct pgrp *destination = pgrp_find_locked(request->pgid);

		if (destination == 0 || destination->pg_session != parent->p_pgrp->pg_session) return PGRP_STATUS_DENIED;

		pgrp_attach_locked(child, destination);
		return PGRP_STATUS_OK;
	}

	case PROC_GROUP_NEW_SESSION: {
		if (pgrp == 0 || *pgrp == 0 || session == 0 || *session == 0) return PGRP_STATUS_NO_MEMORY;

		pgrp_reclaim_t unused = {0};

		pgrp_new_session_locked(child, *pgrp, *session, &unused);
		*pgrp = 0;
		*session = 0;
		return PGRP_STATUS_OK;
	}

	default:
		return PGRP_STATUS_INVALID;
	}
}

void
pgrp_new_session_locked(proc_t proc, struct pgrp *pgrp, struct session *session, pgrp_reclaim_t *reclaim)
{
	pgrp_detach_locked(proc, reclaim);

	session->s_sid = proc->p_ident.pid;
	session->s_leader = proc;
	session->s_count = 0U;

	pgrp_create_locked(pgrp, proc->p_ident.pid, session);
	pgrp_attach_locked(proc, pgrp);
}

void
pgrp_leave_locked(proc_t proc, pgrp_reclaim_t *reclaim)
{
	if (proc == 0) return;

	pgrp_leader_exit_locked(proc);
	pgrp_detach_locked(proc, reclaim);
}

void
pgrp_leader_exit_locked(proc_t proc)
{
	if (proc == 0 || proc->p_pgrp == 0) return;

	struct session *session = proc->p_pgrp->pg_session;

	if (session->s_leader == proc) session->s_leader = 0;
}

/* The live process a pid argument names: 0 is the caller. No reference: the caller holds the lock. */
static proc_t
pgrp_target_locked(proc_t caller, proc_id_t pid)
{
	return pid == 0U || pid == caller->p_ident.pid ? caller : proc_find_live_locked(pid);
}

pgrp_status_t
proc_getpgid(proc_t caller, proc_id_t pid, proc_id_t *pgid)
{
	if (caller == 0 || pgid == 0) return PGRP_STATUS_INVALID;

	pgrp_status_t status = PGRP_STATUS_NO_SUCH_PROCESS;

	proc_table_lock();

	proc_t target = pgrp_target_locked(caller, pid);

	if (target != 0 && target->p_pgrp != 0) {
		*pgid = target->p_pgrp->pg_id;
		status = PGRP_STATUS_OK;
	}

	proc_table_unlock();
	return status;
}

pgrp_status_t
proc_getsid(proc_t caller, proc_id_t pid, proc_id_t *sid)
{
	if (caller == 0 || sid == 0) return PGRP_STATUS_INVALID;

	pgrp_status_t status = PGRP_STATUS_NO_SUCH_PROCESS;

	proc_table_lock();

	proc_t target = pgrp_target_locked(caller, pid);

	if (target != 0 && target->p_pgrp != 0) {
		*sid = target->p_pgrp->pg_session->s_sid;
		status = PGRP_STATUS_OK;
	}

	proc_table_unlock();
	return status;
}

bool
proc_is_session_leader(proc_t proc)
{
	if (proc == 0) return false;

	proc_table_lock();

	bool leader = proc->p_pgrp != 0 && proc->p_pgrp->pg_session->s_leader == proc;

	proc_table_unlock();
	return leader;
}

/* setpgid with the lock held. *fresh is a prepared group, set to 0 if it was used. */
static pgrp_status_t
proc_setpgid_locked(proc_t caller, proc_id_t pid, proc_id_t pgid, struct pgrp **fresh, pgrp_reclaim_t *reclaim)
{
	proc_t target = pgrp_target_locked(caller, pid);

	if (target == 0 || target->p_pgrp == 0 || caller->p_pgrp == 0) return PGRP_STATUS_NO_SUCH_PROCESS;

	if (target != caller) {
		if (target->p_pptr != caller) return PGRP_STATUS_NO_SUCH_PROCESS;

		if ((target->p_flag & PROC_FLAG_EXECED) != 0U) return PGRP_STATUS_DENIED;
	}

	struct session *session = target->p_pgrp->pg_session;

	if (session != caller->p_pgrp->pg_session || session->s_leader == target) return PGRP_STATUS_DENIED;

	if (pgid == 0U) pgid = target->p_ident.pid;

	if (target->p_pgrp->pg_id == pgid) return PGRP_STATUS_OK;

	struct pgrp *destination = pgrp_find_locked(pgid);

	if (destination != 0) {
		if (destination->pg_session != session) return PGRP_STATUS_DENIED;
	} else {
		/* A group that does not exist yet can only be the one a process names after itself. */
		if (pgid != target->p_ident.pid) return PGRP_STATUS_DENIED;

		destination = *fresh;
		*fresh = 0;

		/*
		 * Created before the process leaves its old group: if that was the
		 * session's last group, the session must not be seen empty in between.
		 */
		pgrp_create_locked(destination, pgid, session);
	}

	pgrp_detach_locked(target, reclaim);
	pgrp_attach_locked(target, destination);
	return PGRP_STATUS_OK;
}

pgrp_status_t
proc_setpgid(proc_t caller, proc_id_t pid, proc_id_t pgid)
{
	if (caller == 0 || pid > PROC_PID_MAX || pgid > PROC_PID_MAX) return PGRP_STATUS_INVALID;

	struct pgrp *fresh;
	pgrp_reclaim_t reclaim = {0};

	if (!pgrp_prepare(&fresh, 0)) return PGRP_STATUS_NO_MEMORY;

	proc_table_lock();

	pgrp_status_t status = proc_setpgid_locked(caller, pid, pgid, &fresh, &reclaim);

	proc_table_unlock();

	pgrp_discard(fresh, 0);
	pgrp_reclaim(&reclaim);
	return status;
}

pgrp_status_t
proc_setsid(proc_t caller, proc_id_t *sid)
{
	if (caller == 0 || sid == 0) return PGRP_STATUS_INVALID;

	struct pgrp *pgrp;
	struct session *session;
	pgrp_reclaim_t reclaim = {0};

	if (!pgrp_prepare(&pgrp, &session)) return PGRP_STATUS_NO_MEMORY;

	pgrp_status_t status = PGRP_STATUS_DENIED;

	proc_table_lock();

	/* Neither leading a group now, nor leaving behind a group still named after it. */
	if (caller->p_pgrp != 0 && caller->p_pgrp->pg_id != caller->p_ident.pid &&
		pgrp_find_locked(caller->p_ident.pid) == 0) {
		pgrp_new_session_locked(caller, pgrp, session, &reclaim);
		pgrp = 0;
		session = 0;
		*sid = caller->p_ident.pid;
		status = PGRP_STATUS_OK;
	}

	proc_table_unlock();

	pgrp_discard(pgrp, session);
	pgrp_reclaim(&reclaim);
	return status;
}

uint32_t
pgrp_members_ref(proc_id_t pgid, proc_t *members, uint32_t capacity)
{
	if (members == 0 || capacity == 0U) return 0U;

	uint32_t count = 0U;

	proc_table_lock();

	struct pgrp *pgrp = pgrp_find_locked(pgid);

	for (proc_t member = pgrp != 0 ? pgrp->pg_members : 0; member != 0 && count < capacity;
		 member = member->p_pglist_next) {
		if (proc_is_live_locked(member) && proc_reference_locked(member)) members[count++] = member;
	}

	proc_table_unlock();
	return count;
}

/* A process that still counts as a member or as a parent: it has not become a zombie. */
static bool
pgrp_present_locked(proc_t proc, proc_t except)
{
	return proc != 0 && proc != except && proc->p_stat != PROC_STATE_UNUSED && proc->p_stat != PROC_STATE_ZOMBIE &&
		   proc->p_stat != PROC_STATE_DEAD;
}

/*
 * True when no member of pgrp has a parent in another group of the same
 * session, counting neither `except` (0 for nobody) as a member nor as a
 * parent.
 */
static bool
pgrp_orphaned_locked(const struct pgrp *pgrp, proc_t except)
{
	for (proc_t member = pgrp->pg_members; member != 0; member = member->p_pglist_next) {
		if (!pgrp_present_locked(member, except)) continue;

		proc_t parent = member->p_pptr;

		if (!pgrp_present_locked(parent, except) || parent->p_pgrp == 0) continue;

		if (parent->p_pgrp != pgrp && parent->p_pgrp->pg_session == pgrp->pg_session) return false;
	}

	return true;
}

static bool
pgrp_has_stopped_locked(const struct pgrp *pgrp, proc_t except)
{
	for (proc_t member = pgrp->pg_members; member != 0; member = member->p_pglist_next) {
		if (member != except && member->p_stat == PROC_STATE_STOPPED) return true;
	}

	return false;
}

bool
proc_pgrp_is_orphaned(proc_t proc)
{
	if (proc == 0) return false;

	proc_table_lock();

	bool orphaned = proc->p_pgrp != 0 && pgrp_orphaned_locked(proc->p_pgrp, 0);

	proc_table_unlock();
	return orphaned;
}

/* Add pgrp to the report if exiting's departure orphans it while it has stopped members. */
static uint32_t
pgrp_report_orphaned_locked(const struct pgrp *pgrp, proc_t exiting, proc_id_t *pgids, uint32_t count,
							uint32_t capacity)
{
	if (pgrp == 0 || count >= capacity) return count;

	for (uint32_t index = 0U; index < count; index++) {
		if (pgids[index] == pgrp->pg_id) return count;
	}

	/* Only a group this exit orphans: one that was orphaned already has been dealt with, or never had anyone. */
	if (pgrp_orphaned_locked(pgrp, 0) || !pgrp_orphaned_locked(pgrp, exiting)) return count;

	if (!pgrp_has_stopped_locked(pgrp, exiting)) return count;

	pgids[count] = pgrp->pg_id;
	return count + 1U;
}

uint32_t
pgrp_orphaned_by_exit_locked(proc_t exiting, proc_id_t *pgids, uint32_t capacity)
{
	if (exiting == 0 || pgids == 0) return 0U;

	/* Its own group loses a member whose parent may have been the group's link to the session. */
	uint32_t count = pgrp_report_orphaned_locked(exiting->p_pgrp, exiting, pgids, 0U, capacity);

	/* Each child's group loses the parent that may have been its link. */
	for (proc_t child = exiting->p_children; child != 0; child = child->p_sibling_next)
		count = pgrp_report_orphaned_locked(child->p_pgrp, exiting, pgids, count, capacity);

	return count;
}
