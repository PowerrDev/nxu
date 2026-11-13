/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/tty_internal.h
 *
 * The terminal object itself, shared by the files of kern/tty and by nothing
 * else. Drivers and the rest of the kernel see only kern/tty/tty.h.
 */

#ifndef NXU_KERN_TTY_TTY_INTERNAL_H
#define NXU_KERN_TTY_TTY_INTERNAL_H

#include <kern/tty/tty.h>

#include <kern/ioready.h>
#include <kern/tty/n_tty.h>
#include <vfs/devfs.h>
#include <kern/lock.h>
#include <kern/sched_prism/waitq.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * tty_queue_t
 *
 * A byte ring. `head` is the oldest byte, `count` how many follow it.
 * Protected by the owning terminal's lock.
 */
typedef struct {
	uint8_t *data;
	uint32_t size;
	uint32_t head;
	uint32_t count;
} tty_queue_t;

/*
 * struct tty
 *
 * Everything below t_lock is protected by it, except the fields marked
 * "fixed", which are set at creation and never change, and the wait queues
 * and notifier, which have their own locks.
 *
 * t_references
 *     The driver's reference, the registration's, one per open file, and
 *     any transient ones. The terminal is freed when it reaches zero.
 *
 * t_opens
 *     Open files. Moves between 0 and 1 only with t_transition set.
 *
 * t_transition
 *     The driver is being opened (first open) or closed (last close). Opens
 *     that arrive meanwhile sleep on t_state until it clears.
 *
 * t_hungup / t_detached
 *     See "State" in tty.h. t_detached implies t_hungup and is never cleared.
 *
 * t_throttled
 *     tty_input refused bytes, or filled the input buffer, since the last
 *     read that took any; the next such read calls the driver's unthrottle
 *     operation.
 *
 * t_stopped
 *     Output is stopped by VSTOP (IXON): tty_output_take hands the driver
 *     nothing until VSTART.
 *
 * t_deferred
 *     TTY_DEFER_* work a line-discipline hook asked for while t_lock was held,
 *     done by tty_run_deferred once it is dropped.
 *
 * t_signals
 *     Signals (1 << number) input has asked for that have not been delivered.
 *     They go to the terminal's foreground process group, which job control
 *     provides; until a terminal has one there is nobody to deliver them to,
 *     and they stay recorded here.
 *
 * t_winsize
 *     The window size (kern/syscall/termios_defs.h).
 *
 * t_session / t_pgrp
 *     The session this is the controlling terminal of and its foreground
 *     process group, 0 for none (kern/tty/tty_jobctl.c). IDs, not pointers:
 *     the process subsystem owns the objects.
 *
 * t_deliver
 *     Signals (1 << number) to post to the foreground group once the lock is
 *     dropped (the signal characters, a window change); see t_signals.
 *
 * t_ldisc / t_input_buffer / t_input_delimiters
 *     The line discipline and the input ring it owns (kern/tty/n_tty.h),
 *     with its termios. The ring replaces a plain input queue.
 *
 * t_registered / t_name
 *     Whether the terminal has a name in /dev, and what it is.
 */
struct tty {
	const tty_driver_operations_t *t_driver; /* fixed */
	void *t_driver_context;					 /* fixed */

	nxu_spinlock_t t_lock;
	uint32_t t_references;
	uint32_t t_opens;
	bool t_transition;
	bool t_hungup;
	bool t_detached;
	bool t_throttled;
	bool t_stopped;
	bool t_registered;
	char t_name[TTY_NAME_MAX + 1U];
	uint32_t t_deferred;
	uint32_t t_signals;
	struct nxu_winsize t_winsize;
	proc_id_t t_session;
	proc_id_t t_pgrp;
	uint32_t t_deliver;

	n_tty_t t_ldisc;
	uint8_t *t_input_buffer;
	uint8_t *t_input_delimiters;
	tty_queue_t t_output;

	waitq_t t_readers; /* waiting for input, or for a hangup */
	waitq_t t_writers; /* waiting for room in the output queue, or for a hangup */
	waitq_t t_state;   /* waiting for t_transition to clear */

	/* Posted when input arrives, output room appears, or the line hangs up. */
	ioready_t t_ready;
};

/*
 * tty_deferred_t
 *
 * What a path that held the lock must do once it is dropped: the TTY_DEFER_*
 * work, and signals to post to a foreground group. Filled by
 * tty_take_deferred, under the lock; carried out by tty_run_deferred.
 */
typedef struct {
	uint32_t work;
	uint32_t signals;
	proc_id_t pgrp;
} tty_deferred_t;

/* Work for after the lock (t_deferred). */
#define TTY_DEFER_START (1U << 0U)		  /* output was queued or restarted: start the driver */
#define TTY_DEFER_POST_WRITE (1U << 1U)	  /* room appeared in the output queue */
#define TTY_DEFER_POST_READ (1U << 2U)	  /* input became readable */
#define TTY_DEFER_UNTHROTTLE (1U << 3U)	  /* a throttled input queue has room again */

static inline uint64_t
tty_lock(tty_t tty)
{
	return nxu_spin_lock_irqsave(&tty->t_lock);
}

static inline void
tty_unlock(tty_t tty, uint64_t state)
{
	nxu_spin_unlock_irqrestore(&tty->t_lock, state);
}

/*
 * The terminal-wide operations the ioctl requests are made of (kern/tty/tty.c).
 * Each takes and drops the terminal's lock itself.
 *
 * tty_change_termios
 *     tty_set_termios, discarding unread input in the same step when
 *     `flush_input` (TIOCSETAF).
 *
 * tty_flush
 *     Discard input, output, or both (NXU_FREAD, NXU_FWRITE).
 *
 * tty_drain
 *     Sleep, interruptibly, until the output queue is empty or the terminal
 *     hangs up. OK, or INTERRUPTED.
 *
 * tty_stop_output
 *     Stop or restart output, as VSTOP and VSTART do.
 */
vfs_status_t tty_change_termios(tty_t tty, const struct nxu_termios *termios, bool flush_input);
void tty_flush(tty_t tty, uint32_t which);
vfs_status_t tty_drain(tty_t tty);
void tty_stop_output(tty_t tty, bool stop);

/*
 * tty_ioctl
 *
 * The ioctl operation of a terminal's files (kern/tty/tty_ioctl.c). Called
 * with no terminal lock held, from the file layer, with `argument` a kernel
 * buffer sized by the request.
 */
vfs_status_t tty_ioctl(tty_t tty, uint32_t command, void *argument);

/* The file operations of every terminal node, and the open behind them (also /dev/tty's, kern/tty/tty_jobctl.c). */
extern const devfs_operations_t g_tty_devfs_operations;
vfs_status_t tty_file_open(tty_t tty, uint32_t flags);

/*
 * Job control, kern/tty/tty_jobctl.c. See the file header there.
 *
 * tty_job_check
 *     May the caller do this to the terminal? OK; INTERRUPTED after posting
 *     SIGTTIN or SIGTTOU to its group (restarted once it is continued); or
 *     IO_ERROR. No lock held.
 *
 * tty_job_open
 *     After a successful open: a session leader with no controlling terminal
 *     acquires this one, unless VFS_OPEN_NOCTTY.
 *
 * tty_job_ioctl
 *     The job-control requests; *handled is false for any other.
 *
 * tty_hangup_session
 *     The line went down: the session loses its terminal, and its leader and
 *     foreground group are told.
 *
 * tty_jobctl_init
 *     Publish /dev/tty.
 */
typedef enum { TTY_JOB_READ, TTY_JOB_WRITE, TTY_JOB_CONFIGURE } tty_job_access_t;

vfs_status_t tty_job_check(tty_t tty, tty_job_access_t access);
void tty_job_open(tty_t tty, uint32_t flags);
vfs_status_t tty_job_ioctl(tty_t tty, uint32_t command, void *argument, bool *handled);
void tty_hangup_session(tty_t tty);
bool tty_jobctl_init(void);

#endif
