/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/tty_jobctl.c
 *
 * Job control on the terminal: a session's controlling terminal, its
 * foreground process group, what a process outside that group may do, and
 * what happens to the session when the line hangs up or its leader exits.
 *
 * Model. The process subsystem owns sessions and groups (kern/process/pgrp.h)
 * and a session holds a reference to its controlling terminal. The terminal
 * holds only identifiers: the session it controls (t_session) and its
 * foreground group (t_pgrp), 0 when there is none. The two sides are changed
 * together, under the process table lock and then the terminal's lock, in that
 * order (pgrp.c calls tty_claim_locked); nothing takes the process table lock
 * while holding a terminal's.
 *
 * Signals (rule 6.3 of doc/architecture/tty-subsystem.md) are posted with
 * signal_post and signal_post_group only, always after both locks are
 * dropped, and never synchronously end the target.
 *
 * Access from the background. A process whose controlling terminal this is
 * and which is not in its foreground group:
 *
 *   reads        gets SIGTTIN, or an I/O error when it ignores or blocks
 *                SIGTTIN or its group is orphaned (nobody could continue it);
 *   writes       with TOSTOP set, gets SIGTTOU, unless it ignores or blocks
 *                SIGTTOU (then the write goes ahead), or an I/O error when its
 *                group is orphaned;
 *   reconfigures (tcsetattr, tcflush, tcdrain, tcflow, tcsetpgrp, the window
 *                size) the same as a write, TOSTOP or not.
 *
 * A signal sent this way is answered with "interrupted", which the system
 * call layer restarts once the process is continued (kern/syscall/syscall.h):
 * a job stopped by SIGTTIN and moved to the foreground simply carries on with
 * its read. A process for which this is not the controlling terminal, or a
 * terminal with no foreground group, is not restricted.
 */

#include <kern/tty/tty_internal.h>

#include <kern/console/console.h>
#include <kern/process/pgrp.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/process/thread.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/syscall/termios_defs.h>
#include <vfs/devfs.h>
#include <vfs/file.h>

#include <stdbool.h>
#include <stdint.h>

/* What tcgetpgrp reports when there is no foreground group: no group can have this ID. */
#define TTY_NO_PGRP ((int32_t)PROC_PID_MAX + 1)

/* ---- the terminal's side, called by kern/process/pgrp.c --------------- */

bool
tty_claim_locked(tty_t tty, proc_id_t sid, proc_id_t pgid)
{
	uint64_t state = tty_lock(tty);
	bool free = tty->t_session == 0U && !tty->t_detached;

	if (free) {
		tty->t_session = sid;
		tty->t_pgrp = pgid;
	}

	tty_unlock(tty, state);
	return free;
}

void
tty_session_gone(tty_t tty, proc_id_t sid)
{
	uint64_t state = tty_lock(tty);

	if (tty->t_session == sid) {
		tty->t_session = 0U;
		tty->t_pgrp = 0U;
	}

	tty_unlock(tty, state);
}

/*
 * Let go of the session `sid` if it is this terminal's, and tell its
 * foreground group: SIGHUP, then SIGCONT so a stopped job wakes to it. The
 * session's own link is cleared by the caller (or already was).
 */
static void
tty_release_session(tty_t tty, proc_id_t sid)
{
	uint64_t state = tty_lock(tty);
	proc_id_t foreground = 0U;

	if (tty->t_session == sid) {
		foreground = tty->t_pgrp;
		tty->t_session = 0U;
		tty->t_pgrp = 0U;
	}

	tty_unlock(tty, state);

	if (foreground != 0U) {
		(void)signal_post_group(foreground, NXU_SIGHUP);
		(void)signal_post_group(foreground, NXU_SIGCONT);
	}
}

void
tty_session_leader_exit(tty_t tty, proc_id_t sid)
{
	tty_release_session(tty, sid);
}

/*
 * tty_hangup_session
 *
 * The line went down (tty_hangup, tty_detach): the session loses its
 * terminal, and its leader (the controlling process) and its foreground group
 * are told. Called with no terminal lock held.
 */
void
tty_hangup_session(tty_t tty)
{
	uint64_t state = tty_lock(tty);
	proc_id_t sid = tty->t_session;
	tty_unlock(tty, state);

	if (sid == 0U) return;

	tty_t link = proc_session_drop_tty(sid, tty);

	tty_release_session(tty, sid);

	proc_t leader = proc_find(sid);

	if (leader != 0) {
		if (proc_is_session_leader(leader)) {
			(void)signal_post(leader, NXU_SIGHUP);
			(void)signal_post(leader, NXU_SIGCONT);
		}

		proc_rele(leader);
	}

	if (link != TTY_NULL) tty_release(link);
}

/* ---- the caller's relation to the terminal ------------------------------ */

/* The caller's job identity, and whether this terminal controls its session. */
static bool
tty_job_of_caller(tty_t tty, proc_job_t *job)
{
	proc_t proc = current_proc();

	if (proc == 0 || proc == proc_kernel() || !proc_job_get(proc, job)) return false;

	bool ours = job->ctty == tty;

	if (job->ctty != TTY_NULL) tty_release(job->ctty);

	job->ctty = TTY_NULL;
	return ours;
}

static bool
tty_signal_ignored_or_blocked(uint32_t signal)
{
	proc_t proc = current_proc();
	thread_t thread = current_thread();
	nxu_sigaction_t action;

	if (signal_get_action(proc, signal, &action) && action.handler == NXU_SIG_IGN) return true;

	return thread != 0 && (thread->sig_blocked & (1U << signal)) != 0U;
}

vfs_status_t
tty_job_check(tty_t tty, tty_job_access_t access)
{
	proc_job_t job;

	if (!tty_job_of_caller(tty, &job)) return VFS_STATUS_OK;

	uint64_t state = tty_lock(tty);
	proc_id_t foreground = tty->t_pgrp;
	bool tostop = (tty->t_ldisc.termios.c_lflag & NXU_TOSTOP) != 0U;
	tty_unlock(tty, state);

	if (foreground == 0U || foreground == job.pgid) return VFS_STATUS_OK;

	if (access == TTY_JOB_READ) {
		if (tty_signal_ignored_or_blocked(NXU_SIGTTIN) || proc_pgrp_is_orphaned(current_proc()))
			return VFS_STATUS_IO_ERROR;

		(void)signal_post_group(job.pgid, NXU_SIGTTIN);
		return VFS_STATUS_INTERRUPTED;
	}

	if (access == TTY_JOB_WRITE && !tostop) return VFS_STATUS_OK;

	if (tty_signal_ignored_or_blocked(NXU_SIGTTOU)) return VFS_STATUS_OK;

	if (proc_pgrp_is_orphaned(current_proc())) return VFS_STATUS_IO_ERROR;

	(void)signal_post_group(job.pgid, NXU_SIGTTOU);
	return VFS_STATUS_INTERRUPTED;
}

/*
 * tty_job_open
 *
 * An open of the terminal has succeeded: a session leader with no
 * controlling terminal acquires this one, unless the open asked not to
 * (VFS_OPEN_NOCTTY) or another session already has it.
 */
void
tty_job_open(tty_t tty, uint32_t flags)
{
	proc_t proc = current_proc();

	if ((flags & VFS_OPEN_NOCTTY) != 0U || proc == 0 || proc == proc_kernel()) return;

	(void)proc_ctty_acquire(proc, tty);
}

bool
tty_job_acquire_for(proc_t proc, tty_t tty)
{
	return proc_ctty_acquire(proc, tty) == PGRP_STATUS_OK;
}

vfs_status_t
tty_job_set_foreground_for(proc_t proc, proc_id_t pgid)
{
	proc_job_t job;

	if (proc == 0 || !proc_job_get(proc, &job)) return VFS_STATUS_INVALID;

	tty_t tty = job.ctty;

	if (tty == TTY_NULL) return VFS_STATUS_NOT_TTY;

	vfs_status_t status = VFS_STATUS_OK;

	if (!pgrp_in_session(pgid, job.sid)) {
		status = VFS_STATUS_DENIED;
	} else {
		uint64_t state = tty_lock(tty);

		if (tty->t_session == job.sid)
			tty->t_pgrp = pgid;
		else
			status = VFS_STATUS_NOT_TTY;

		tty_unlock(tty, state);
	}

	tty_release(tty);
	return status;
}

/* ---- the requests ----------------------------------------------------- */

/*
 * tty_job_ioctl
 *
 * TIOCSCTTY, TIOCNOTTY, TIOCGPGRP, TIOCSPGRP and TIOCGSID; *handled is false
 * for any other request. Called with no lock held.
 */
vfs_status_t
tty_job_ioctl(tty_t tty, uint32_t command, void *argument, bool *handled)
{
	proc_job_t job;
	bool ours;

	*handled = true;

	switch (command) {
	case NXU_TIOCSCTTY:
		if (current_proc() == 0 || proc_ctty_acquire(current_proc(), tty) != PGRP_STATUS_OK) return VFS_STATUS_DENIED;

		return VFS_STATUS_OK;

	case NXU_TIOCNOTTY:
		ours = tty_job_of_caller(tty, &job);

		if (!ours) return VFS_STATUS_NOT_TTY;

		/* Only the session leader gives the terminal up for the session; for anyone else this is a no-op. */
		if (job.leader) {
			tty_t link = proc_session_drop_tty(job.sid, tty);

			tty_release_session(tty, job.sid);
			if (link != TTY_NULL) tty_release(link);
		}

		return VFS_STATUS_OK;

	case NXU_TIOCGPGRP:
	case NXU_TIOCGSID: {
		if (!tty_job_of_caller(tty, &job)) return VFS_STATUS_NOT_TTY;

		uint64_t state = tty_lock(tty);
		proc_id_t value = command == NXU_TIOCGPGRP ? tty->t_pgrp : tty->t_session;
		tty_unlock(tty, state);

		*(int32_t *)argument = value != 0U ? (int32_t)value : TTY_NO_PGRP;
		return VFS_STATUS_OK;
	}

	case NXU_TIOCSPGRP: {
		int32_t pgid = *(const int32_t *)argument;

		if (!tty_job_of_caller(tty, &job)) return VFS_STATUS_NOT_TTY;
		if (pgid <= 0 || (uint32_t)pgid > PROC_PID_MAX) return VFS_STATUS_INVALID;

		vfs_status_t status = tty_job_check(tty, TTY_JOB_CONFIGURE);

		if (status != VFS_STATUS_OK) return status;

		return tty_job_set_foreground_for(current_proc(), (proc_id_t)pgid);
	}

	default:
		*handled = false;
		return VFS_STATUS_NOT_TTY;
	}
}

/* ---- /dev/tty ----------------------------------------------------------- */

/*
 * /dev/tty is whichever terminal controls the opener's session: the open is
 * that terminal's own (tty_open_file), so the file is one of the terminal's
 * like any other, revoked with them.
 */
static vfs_status_t
tty_dev_tty_open(void *context, uint32_t flags, devfs_clone_t *clone)
{
	(void)context;

	proc_t proc = current_proc();
	proc_job_t job;

	if (proc == 0 || !proc_job_get(proc, &job) || job.ctty == TTY_NULL) return VFS_STATUS_NO_DEVICE;

	vfs_status_t status = tty_open_file(job.ctty, flags | VFS_OPEN_NOCTTY, clone);

	/* The open file holds its own reference now. */
	tty_release(job.ctty);
	return status;
}

static const devfs_operations_t g_dev_tty_operations = {.open = tty_dev_tty_open};

bool
tty_jobctl_init(void)
{
	return devfs_register_device("tty", &g_dev_tty_operations, 0);
}
