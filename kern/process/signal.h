/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/signal.h
 *
 * POSIX-flavoured signals, machine-independent half: dispositions, pending
 * and blocked sets, who may signal whom, the default actions, and stopping
 * and continuing a process. The machine-dependent half -- pushing a handler
 * frame onto the user stack and restoring it -- is <kern/machine/user.h>,
 * driven from the trap return path.
 *
 * Model:
 *
 *   - A signal is sent to a process and stays pending on it (proc::
 *     p_sigpending) until one of its threads, returning to user mode, does
 *     not block it. The blocked mask is per thread (thread::sig_blocked).
 *
 *   - Each signal has one disposition per process: the default action,
 *     ignore, or a user handler (sigaction). SIGKILL and SIGSTOP cannot be
 *     caught, blocked or ignored.
 *
 *   - The default action is to terminate the process (its exit status
 *     records the signal; see NXU_EXIT_KILLED_SIGNAL), except: SIGCHLD and
 *     SIGWINCH are ignored; SIGSTOP, SIGTSTP, SIGTTIN and SIGTTOU stop the
 *     process; SIGCONT continues it.
 *
 *   - A process acts on its own signals. Whatever a signal does to a
 *     process -- run a handler, stop it, end it -- is done by one of that
 *     process's threads, in signal_take, as it is about to return to user
 *     mode. Sending is therefore only ever "mark it pending and make sure the
 *     target looks": see signal_post. The one exception is kill(2) ending
 *     another process, described at signal_send.
 *
 *   - Stopping. A stopped process has every thread parked at its return to
 *     user mode; a thread that is asleep in the kernel stays asleep and parks
 *     when its system call finishes. SIGCONT continues a stopped process the
 *     moment it is sent, whatever its disposition, and discards pending stop
 *     signals; a stop signal discards a pending SIGCONT. SIGKILL ends a
 *     stopped process without continuing it. The parent can learn of both
 *     changes from wait (NXU_WAIT_UNTRACED, NXU_WAIT_CONTINUED_OPTION) and is
 *     sent SIGCHLD.
 *
 *   - Who may signal whom follows credentials: root may signal anyone; anyone
 *     else may signal a process whose real or saved user ID equals the
 *     sender's real or effective one; SIGCONT may also be sent to any process
 *     in the sender's session. The kernel process is never a target, and
 *     PID 1 only takes signals it has installed a handler for.
 */

#ifndef NXU_KERN_PROCESS_SIGNAL_H
#define NXU_KERN_PROCESS_SIGNAL_H

#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/syscall/syscall_defs.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	SIGNAL_SEND_OK,
	SIGNAL_SEND_INVALID,
	SIGNAL_SEND_NO_SUCH_PROCESS,
	SIGNAL_SEND_DENIED
} signal_send_result_t;

typedef enum {
	SIGNAL_DEFAULT_TERMINATE,
	SIGNAL_DEFAULT_IGNORE,
	SIGNAL_DEFAULT_STOP,
	SIGNAL_DEFAULT_CONTINUE
} signal_default_t;

/* True for 1 <= signal < NXU_NSIG. */
bool signal_valid(uint32_t signal);

signal_default_t signal_default_action(uint32_t signal);

/*
 * signal_set_action / signal_get_action
 *
 * Read or replace one disposition. Replacing fails for SIGKILL and SIGSTOP.
 * A handler is NXU_SIG_DFL, NXU_SIG_IGN or a user address below the
 * user-space limit.
 */
bool signal_set_action(proc_t proc, uint32_t signal, const nxu_sigaction_t *action);
bool signal_get_action(proc_t proc, uint32_t signal, nxu_sigaction_t *action);

/*
 * signal_inherit
 *
 * fork: child gets parent's dispositions and no pending signals.
 */
void signal_inherit(proc_t child, proc_t parent);

/*
 * signal_reset_for_exec
 *
 * exec: a caught signal reverts to the default -- the handler's code is
 * gone -- while an ignored one stays ignored. Pending signals survive.
 */
void signal_reset_for_exec(proc_t proc);

/*
 * signal_post
 *
 * Make signal pending on target and see that target notices: threads of it
 * in an interruptible sleep are woken, threads running in user mode are
 * kicked into the kernel. A signal the target ignores is dropped. SIGCONT
 * also continues a stopped target, here and now.
 *
 * This is the sending primitive for the kernel's own use (the terminal, the
 * process subsystem). It checks no permission. It never ends, stops or
 * otherwise acts on the target: the target does that itself, later, in its
 * own context. So it never sleeps and never closes anything, and it is safe
 * to call wherever tearing a process down would not be. It takes the process
 * table lock and p_siglock, neither with interrupts masked against it, so it
 * is called from thread context, with no spinlock held that an interrupt
 * handler or the process subsystem could want.
 *
 * A thread of the target that is in an uninterruptible sleep acts on the
 * signal only when that sleep ends.
 *
 * False if target is not a live user process or signal is not valid.
 */
bool signal_post(proc_t target, uint32_t signal);

/*
 * signal_post_group
 *
 * signal_post to every live member of process group pgid. Returns how many
 * were posted to; 0 if the group does not exist.
 */
uint32_t signal_post_group(proc_id_t pgid, uint32_t signal);

/*
 * signal_send / signal_send_group
 *
 * kill(2) and killpg(2): send signal (0 only probes for existence and
 * permission) on behalf of sender to process pid, or to every member of group
 * pgid (0: the sender's own group). Enforces the permission rules above; for
 * a group, the call succeeds if at least one member could be signalled.
 *
 * Unlike signal_post, a signal that ends another process by default action
 * (or SIGKILL) ends it before this returns, with proc_exit, rather than
 * waiting for the target to reach the edge of the kernel: a thread sleeping
 * uninterruptibly might not get there. That tears a process down, so these
 * two are only for a system call handler, which holds no locks. Kernel code
 * that needs to signal uses signal_post.
 */
signal_send_result_t signal_send(proc_t sender, proc_id_t pid, uint32_t signal);
signal_send_result_t signal_send_group(proc_t sender, proc_id_t pgid, uint32_t signal);

/*
 * signal_take
 *
 * What the calling thread must do about signals before it returns to user
 * mode. Called by the machine-dependent trap return path, with no locks held,
 * for a thread of a user process. It first parks the thread for as long as
 * the process is stopped, then takes pending signals in turn, carrying out
 * the ones that need no user context (ignore, stop, continue) itself, until
 * one of:
 *
 *   SIGNAL_TAKE_NONE       nothing more to do: return to user mode.
 *
 *   SIGNAL_TAKE_HANDLER    *signal is caught: build a handler frame for
 *                          *action (machine_user_signal_push) and set the
 *                          thread's mask (signal_blocked_mask_for_handler).
 *                          If the frame cannot be built, end the process as
 *                          for SIGSEGV.
 *
 *   SIGNAL_TAKE_TERMINATE  *signal ends the process: call
 *                          proc_exit_current(NXU_EXIT_KILLED_SIGNAL(*signal))
 *                          and unwind.
 */
typedef enum { SIGNAL_TAKE_NONE, SIGNAL_TAKE_HANDLER, SIGNAL_TAKE_TERMINATE } signal_take_t;

signal_take_t signal_take(proc_t proc, thread_t thread, uint32_t *signal, nxu_sigaction_t *action);

/*
 * signal_dequeue
 *
 * Take the lowest-numbered pending signal that thread does not block,
 * clearing it from the pending set, or 0 when there is none. SIGKILL and
 * SIGSTOP are never blocked.
 */
uint32_t signal_dequeue(proc_t proc, thread_t thread);

/*
 * signal_pending_for
 *
 * True if a signal is waiting that thread would take -- what a system call
 * about to sleep checks first, so it never sleeps through one.
 */
bool signal_pending_for(proc_t proc, thread_t thread);

/*
 * signal_blocked_mask_for_handler
 *
 * The mask a thread runs a handler for signal with: what it already
 * blocked, the signal itself (unless NXU_SA_NODEFER) and the sigaction's
 * extra mask -- never SIGKILL or SIGSTOP.
 */
uint32_t signal_blocked_mask_for_handler(uint32_t current_mask, uint32_t signal, const nxu_sigaction_t *action);

/*
 * signal_sanitize_mask
 *
 * Strip the bits a mask may not carry: bit 0 (no signal 0), SIGKILL and
 * SIGSTOP.
 */
uint32_t signal_sanitize_mask(uint32_t mask);

/*
 * signal_notify_parent_of_exit
 *
 * SIGCHLD to parent when a child exits, if the parent has a handler for it.
 * The default disposition ignores SIGCHLD, so a parent that never asked
 * costs nothing. Called from proc_exit with the process table lock held.
 */
void signal_notify_parent_of_exit_locked(proc_t parent);

#endif
