/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/signal.h
 *
 * POSIX-flavoured signals, machine-independent half: dispositions, pending
 * and blocked sets, who may signal whom, and the default actions. The
 * machine-dependent half -- pushing a handler frame onto the user stack and
 * restoring it -- is <kern/machine/user.h>, driven from the trap return path.
 *
 * Model:
 *
 *   - A signal is sent to a process and stays pending on it (proc::
 *     p_sigpending) until one of its threads, returning to user mode, does
 *     not block it. The blocked mask is per thread (thread::sig_blocked).
 *
 *   - Each signal has one disposition per process: the default action,
 *     ignore, or a user handler (sigaction). SIGKILL cannot be caught,
 *     blocked or ignored.
 *
 *   - The default action is to terminate the process (its exit status
 *     records the signal; see NXU_EXIT_KILLED_SIGNAL) except for SIGCHLD,
 *     which is ignored. There is no stop/continue: SIGSTOP and SIGCONT are
 *     refused.
 *
 *   - Sending is limited to yourself and your descendants. There are no
 *     credentials yet, so this is what keeps an ordinary process from
 *     signalling PID 1 or a sibling service.
 *
 *   - A signal that terminates a process other than the sender does so at
 *     once; the sender's own (and any caught) signal is delivered as it
 *     returns to user mode.
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
	SIGNAL_SEND_DENIED,
	SIGNAL_SEND_NOT_SUPPORTED
} signal_send_result_t;

typedef enum {
	SIGNAL_DEFAULT_TERMINATE,
	SIGNAL_DEFAULT_IGNORE
} signal_default_t;

/* True for 1 <= signal < NXU_NSIG. */
bool signal_valid(uint32_t signal);

signal_default_t signal_default_action(uint32_t signal);

/*
 * signal_set_action / signal_get_action
 *
 * Read or replace one disposition. Replacing fails for SIGKILL and for
 * SIGSTOP/SIGCONT (which this kernel does not implement). A handler is
 * NXU_SIG_DFL, NXU_SIG_IGN or a user address below the user-space limit.
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
 * signal_send
 *
 * Send signal (0 only probes for existence and permission) to pid on behalf
 * of sender. Enforces the descendants-only rule. If the disposition is the
 * default terminate and the target is not the sender, the target is
 * terminated before this returns; otherwise the signal becomes pending.
 */
signal_send_result_t signal_send(proc_t sender, proc_id_t pid, uint32_t signal);

/*
 * signal_dequeue
 *
 * Take the lowest-numbered pending signal that thread does not block,
 * clearing it from the pending set, or 0 when there is none. SIGKILL is
 * never blocked.
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
 * extra mask -- never SIGKILL.
 */
uint32_t signal_blocked_mask_for_handler(uint32_t current_mask, uint32_t signal, const nxu_sigaction_t *action);

/*
 * signal_sanitize_mask
 *
 * Strip the bits a mask may not carry: bit 0 (no signal 0) and SIGKILL.
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
