/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/signal.c
 *
 * See kern/process/signal.h.
 *
 * Locking. Two locks, always in this order:
 *
 *   - The process table lock (kern/process/proc_internal.h) protects whether
 *     a process is stopped (p_stat), why (p_stop_signal) and what its parent
 *     has not been told yet (p_jobevent).
 *
 *   - proc->p_siglock protects p_sigact, p_sigpending and p_cont_seq. It is a
 *     leaf spinlock, taken with interrupts masked; nothing is called while it
 *     is held. A struct copy of a disposition would otherwise tear against
 *     sigaction() on another CPU. signal_pending_for() is the one lock-free
 *     reader: a single atomic word, used only as a hint before sleeping.
 *
 * Nothing is woken, kicked or signalled with either lock held: each routine
 * decides under the locks, drops them, and then acts.
 *
 * Stopping and continuing race, and p_cont_seq settles it. A thread takes a
 * stop signal off the pending set in one critical section and stops the
 * process in a later one; a SIGCONT sent in between finds the process not
 * yet stopped and would be lost, leaving it stopped for ever. Every SIGCONT
 * bumps p_cont_seq; the dequeue reports the value it saw, and the stop goes
 * ahead only if it is unchanged. A SIGCONT is therefore either before the
 * dequeue (it discarded the stop signal) or cancels the stop.
 */

#include <kern/process/signal.h>

#include <kern/lock.h>
#include <kern/process/pgrp.h>
#include <kern/process/proc_internal.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/machine/vm_param.h>

#include <stdbool.h>
#include <stdint.h>

#define SIGNAL_BIT(signal) (1U << (signal))

bool
signal_valid(uint32_t signal)
{
	return signal >= 1U && signal < NXU_NSIG;
}

signal_default_t
signal_default_action(uint32_t signal)
{
	switch (signal) {
	case NXU_SIGCHLD:
	case NXU_SIGWINCH:
		return SIGNAL_DEFAULT_IGNORE;
	case NXU_SIGSTOP:
	case NXU_SIGTSTP:
	case NXU_SIGTTIN:
	case NXU_SIGTTOU:
		return SIGNAL_DEFAULT_STOP;
	case NXU_SIGCONT:
		return SIGNAL_DEFAULT_CONTINUE;
	default:
		return SIGNAL_DEFAULT_TERMINATE;
	}
}

uint32_t
signal_sanitize_mask(uint32_t mask)
{
	return mask & ~(1U | NXU_SIG_UNCATCHABLE_MASK);
}

bool
signal_set_action(proc_t proc, uint32_t signal, const nxu_sigaction_t *action)
{
	if (proc == 0 || action == 0 || !signal_valid(signal) || (SIGNAL_BIT(signal) & NXU_SIG_UNCATCHABLE_MASK) != 0U) {
		return false;
	}

	if (action->handler > NXU_SIG_IGN) {
		if (action->handler >= VM_MAX_USER_ADDRESS) return false;

		if (action->restorer == 0ULL || action->restorer >= VM_MAX_USER_ADDRESS) return false;
	}

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	proc->p_sigact[signal] = *action;
	proc->p_sigact[signal].mask = signal_sanitize_mask(action->mask);

	/* Ignoring a signal discards one that was already waiting. */
	if (action->handler == NXU_SIG_IGN) proc->p_sigpending &= ~SIGNAL_BIT(signal);

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);

	return true;
}

bool
signal_get_action(proc_t proc, uint32_t signal, nxu_sigaction_t *action)
{
	if (proc == 0 || action == 0 || !signal_valid(signal)) return false;

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	*action = proc->p_sigact[signal];

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);
	return true;
}

void
signal_inherit(proc_t child, proc_t parent)
{
	if (child == 0 || parent == 0) return;

	/* The child is not visible to anyone yet; only the parent's table can change under us. */
	uint64_t irq_state = nxu_spin_lock_irqsave(&parent->p_siglock);

	for (uint32_t signal = 0U; signal < NXU_NSIG; signal++) {
		child->p_sigact[signal] = parent->p_sigact[signal];
	}

	nxu_spin_unlock_irqrestore(&parent->p_siglock, irq_state);

	child->p_sigpending = 0U;
}

void
signal_reset_for_exec(proc_t proc)
{
	if (proc == 0) return;

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	for (uint32_t signal = 0U; signal < NXU_NSIG; signal++) {
		if (proc->p_sigact[signal].handler > NXU_SIG_IGN) {
			proc->p_sigact[signal] = (nxu_sigaction_t){0};
		}
	}

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);
}

bool
signal_pending_for(proc_t proc, thread_t thread)
{
	if (proc == 0 || thread == 0) return false;

	return (__atomic_load_n(&proc->p_sigpending, __ATOMIC_ACQUIRE) &
			(~thread->sig_blocked | NXU_SIG_UNCATCHABLE_MASK)) != 0U;
}

/*
 * signal_interrupt_sleepers
 *
 * A signal just became pending on proc: cut short the interruptible sleep of
 * every thread that would take it, so its system call returns "interrupted"
 * and the thread reaches signal_take instead of the signal waiting behind the
 * sleep.
 */
static void
signal_interrupt_sleepers(proc_t proc, uint32_t signal)
{
	/*
	 * Walk the threads with references, not through the raw list: a thread of
	 * this process can exit on another CPU at any moment and be reclaimed. Each
	 * one that would take the signal is also kicked if it is running, so a signal
	 * for a thread in user mode on another CPU is acted on at its next return
	 * to user mode (one interrupt away) rather than up to a timer tick later.
	 */
	for (thread_t thread = task_first_thread_ref(&proc->p_task); thread != 0;) {
		if ((thread->sig_blocked & SIGNAL_BIT(signal)) == 0U || (SIGNAL_BIT(signal) & NXU_SIG_UNCATCHABLE_MASK) != 0U) {
			(void)waitq_interrupt(thread);
			sched_kick_thread(thread);
		}

		thread_t next = thread_next_task_thread_ref(thread);

		thread_deallocate(thread);
		thread = next;
	}
}

/* Bring every thread of proc that is running in user mode into the kernel, where it will find the process stopped. */
static void
signal_kick_threads(proc_t proc)
{
	for (thread_t thread = task_first_thread_ref(&proc->p_task); thread != 0;) {
		sched_kick_thread(thread);

		thread_t next = thread_next_task_thread_ref(thread);

		thread_deallocate(thread);
		thread = next;
	}
}

/* Tell parent (a reference the caller holds, or 0) that a child stopped or continued. */
static void
signal_notify_parent(proc_t parent)
{
	if (parent == 0) return;

	(void)signal_post(parent, NXU_SIGCHLD);
	waitq_wake_all(&parent->p_waitq);
}

bool
signal_post(proc_t target, uint32_t signal)
{
	if (target == 0 || target == proc_kernel() || !signal_valid(signal)) return false;

	bool pended = false;
	bool continued = false;
	proc_t parent = 0;

	proc_table_lock();

	if (!proc_is_live_locked(target)) {
		proc_table_unlock();
		return false;
	}

	uint64_t irq_state = nxu_spin_lock_irqsave(&target->p_siglock);

	uint64_t handler = target->p_sigact[signal].handler;

	if (signal == NXU_SIGCONT) {
		/* Continuing happens when the signal is sent, whatever is done with the signal itself. */
		target->p_cont_seq++;
		target->p_sigpending &= ~NXU_SIG_STOP_MASK;

		if (target->p_stat == PROC_STATE_STOPPED) {
			__atomic_store_n(&target->p_stat, PROC_STATE_RUNNING, __ATOMIC_RELEASE);
			target->p_jobevent = PROC_JOBEVENT_CONTINUED;
			continued = true;
		}
	} else if ((SIGNAL_BIT(signal) & NXU_SIG_STOP_MASK) != 0U) {
		target->p_sigpending &= ~SIGNAL_BIT(NXU_SIGCONT);
	}

	bool uncatchable = (SIGNAL_BIT(signal) & NXU_SIG_UNCATCHABLE_MASK) != 0U;
	signal_default_t by_default = signal_default_action(signal);

	/* Nothing would ever be done with these, so they are not recorded. */
	bool dropped = !uncatchable && (handler == NXU_SIG_IGN || (handler == NXU_SIG_DFL && (by_default == SIGNAL_DEFAULT_IGNORE ||
																						   by_default == SIGNAL_DEFAULT_CONTINUE)));

	if (!dropped) {
		target->p_sigpending |= SIGNAL_BIT(signal);
		pended = true;
	}

	nxu_spin_unlock_irqrestore(&target->p_siglock, irq_state);

	if (continued && target->p_pptr != 0 && proc_reference_locked(target->p_pptr)) parent = target->p_pptr;

	proc_table_unlock();

	if (continued) {
		waitq_wake_all(&target->p_stopq);
		signal_notify_parent(parent);
	}

	if (parent != 0) proc_rele(parent);

	if (pended) {
		/* A stopped process still has to die: its parked threads look again. */
		if (signal == NXU_SIGKILL) waitq_wake_all(&target->p_stopq);

		signal_interrupt_sleepers(target, signal);
	}

	return true;
}

uint32_t
signal_post_group(proc_id_t pgid, uint32_t signal)
{
	proc_t members[PROC_MAX];
	uint32_t count = pgrp_members_ref(pgid, members, PROC_MAX);
	uint32_t posted = 0U;

	for (uint32_t index = 0U; index < count; index++) {
		if (signal_post(members[index], signal)) posted++;

		proc_rele(members[index]);
	}

	return posted;
}

/* The permission rules of kern/process/signal.h. */
static bool
signal_may_send(proc_t sender, proc_t target, uint32_t signal)
{
	if (target == proc_kernel()) return false;
	if (sender == proc_kernel()) return true;

	bool allowed = sender == target;

	if (!allowed) {
		const struct ucred *from = proc_ucred(sender);
		const struct ucred *to = proc_ucred(target);

		allowed = ucred_is_root(from) || from->cr_ruid == to->cr_ruid || from->cr_ruid == to->cr_svuid ||
				  from->cr_uid == to->cr_ruid || from->cr_uid == to->cr_svuid;
	}

	if (!allowed && signal == NXU_SIGCONT) {
		proc_id_t sender_sid;
		proc_id_t target_sid;

		allowed = proc_getsid(sender, 0U, &sender_sid) == PGRP_STATUS_OK &&
				  proc_getsid(sender, target->p_ident.pid, &target_sid) == PGRP_STATUS_OK && sender_sid == target_sid;
	}

	if (!allowed) return false;

	/* PID 1 is not ended, stopped or surprised by anyone: it takes only what it has a handler for. */
	if (target == proc_initproc() && target != sender && signal != 0U) {
		nxu_sigaction_t action;

		if (!signal_get_action(target, signal, &action) || action.handler <= NXU_SIG_IGN) return false;
	}

	return true;
}

/* kill(2) on one process the caller holds a reference to. */
static signal_send_result_t
signal_send_to(proc_t sender, proc_t target, uint32_t signal)
{
	if (!signal_may_send(sender, target, signal)) return SIGNAL_SEND_DENIED;

	if (signal == 0U) return SIGNAL_SEND_OK;

	nxu_sigaction_t action;

	(void)signal_get_action(target, signal, &action);

	bool terminate = signal == NXU_SIGKILL ||
					 (action.handler == NXU_SIG_DFL && signal_default_action(signal) == SIGNAL_DEFAULT_TERMINATE);

	if (terminate && target != sender) {
		/*
		 * Another process being ended: end it now (its threads may be running
		 * on other CPUs; proc_exit terminates them, waits until they are off
		 * their CPUs and reclaims everything), rather than wait for a return to
		 * user mode that a thread in an uninterruptible sleep might never make.
		 */
		(void)proc_exit(target, NXU_EXIT_KILLED_SIGNAL(signal));
	} else {
		(void)signal_post(target, signal);
	}

	return SIGNAL_SEND_OK;
}

signal_send_result_t
signal_send(proc_t sender, proc_id_t pid, uint32_t signal)
{
	if (sender == 0) return SIGNAL_SEND_INVALID;

	if (signal != 0U && !signal_valid(signal)) return SIGNAL_SEND_INVALID;

	/* The kernel process is not a target. */
	if (pid == PROC_PID_KERNEL) return SIGNAL_SEND_DENIED;

	proc_t target = proc_find(pid);

	if (target == 0) return SIGNAL_SEND_NO_SUCH_PROCESS;

	signal_send_result_t result = signal_send_to(sender, target, signal);

	proc_rele(target);
	return result;
}

signal_send_result_t
signal_send_group(proc_t sender, proc_id_t pgid, uint32_t signal)
{
	if (sender == 0) return SIGNAL_SEND_INVALID;

	if (signal != 0U && !signal_valid(signal)) return SIGNAL_SEND_INVALID;

	if (pgid == 0U && proc_getpgid(sender, 0U, &pgid) != PGRP_STATUS_OK) return SIGNAL_SEND_NO_SUCH_PROCESS;

	proc_t members[PROC_MAX];
	uint32_t count = pgrp_members_ref(pgid, members, PROC_MAX);

	if (count == 0U) return SIGNAL_SEND_NO_SUCH_PROCESS;

	uint32_t sent = 0U;
	bool to_self = false;

	for (uint32_t index = 0U; index < count; index++) {
		/* The sender goes last: a signal that ends or stops it must not cut the loop short. */
		if (members[index] == sender) {
			to_self = true;
		} else if (signal_send_to(sender, members[index], signal) == SIGNAL_SEND_OK) {
			sent++;
		}

		proc_rele(members[index]);
	}

	if (to_self && signal_send_to(sender, sender, signal) == SIGNAL_SEND_OK) sent++;

	return sent != 0U ? SIGNAL_SEND_OK : SIGNAL_SEND_DENIED;
}

/* signal_dequeue that also reports p_cont_seq as it was when the signal was taken. */
static uint32_t
signal_dequeue_seq(proc_t proc, thread_t thread, uint32_t *cont_seq)
{
	if (cont_seq != 0) *cont_seq = 0U;

	if (proc == 0 || thread == 0) return 0U;

	if (!signal_pending_for(proc, thread)) return 0U;

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	uint32_t deliverable = proc->p_sigpending & (~thread->sig_blocked | NXU_SIG_UNCATCHABLE_MASK);
	uint32_t taken = 0U;

	if ((deliverable & SIGNAL_BIT(NXU_SIGKILL)) != 0U) {
		/* Nothing else matters once the process is to be killed. */
		taken = NXU_SIGKILL;
	} else {
		for (uint32_t signal = 1U; signal < NXU_NSIG && deliverable != 0U; signal++) {
			if ((deliverable & SIGNAL_BIT(signal)) == 0U) continue;

			taken = signal;
			break;
		}
	}

	if (taken != 0U) proc->p_sigpending &= ~SIGNAL_BIT(taken);

	if (cont_seq != 0) *cont_seq = proc->p_cont_seq;

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);
	return taken;
}

uint32_t
signal_dequeue(proc_t proc, thread_t thread)
{
	return signal_dequeue_seq(proc, thread, 0);
}

/*
 * signal_stop_self
 *
 * The default action of a stop signal the calling thread just took: mark the
 * process stopped and tell its parent. The thread itself parks when
 * signal_take loops round. cont_seq is p_cont_seq from the dequeue; see the
 * head of this file.
 */
static void
signal_stop_self(proc_t proc, uint32_t signal, uint32_t cont_seq)
{
	/*
	 * A stop asked for from the terminal does nothing in an orphaned process
	 * group: nobody is left who could continue it. SIGSTOP always stops.
	 */
	if (signal != NXU_SIGSTOP && proc_pgrp_is_orphaned(proc)) return;

	bool stopped = false;
	proc_t parent = 0;

	proc_table_lock();

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	if (proc->p_cont_seq == cont_seq && proc_is_live_locked(proc) && proc->p_stat != PROC_STATE_STOPPED) {
		__atomic_store_n(&proc->p_stat, PROC_STATE_STOPPED, __ATOMIC_RELEASE);
		proc->p_stop_signal = signal;
		proc->p_jobevent = PROC_JOBEVENT_STOPPED;
		stopped = true;
	}

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);

	if (stopped && proc->p_pptr != 0 && proc_reference_locked(proc->p_pptr)) parent = proc->p_pptr;

	proc_table_unlock();

	if (!stopped) return;

	signal_kick_threads(proc);
	signal_notify_parent(parent);

	if (parent != 0) proc_rele(parent);
}

/* Sleep for as long as the process is stopped, unless it is to be killed. */
static void
signal_park_while_stopped(proc_t proc)
{
	for (;;) {
		uint32_t seq = waitq_seq(&proc->p_stopq);

		if (__atomic_load_n(&proc->p_stat, __ATOMIC_ACQUIRE) != PROC_STATE_STOPPED) return;

		if ((__atomic_load_n(&proc->p_sigpending, __ATOMIC_ACQUIRE) & SIGNAL_BIT(NXU_SIGKILL)) != 0U) return;

		/*
		 * Not interruptible: only SIGCONT and SIGKILL end a stop, and both wake
		 * this queue after changing what the two tests above look at.
		 */
		(void)waitq_block_seq(&proc->p_stopq, seq, false);
	}
}

signal_take_t
signal_take(proc_t proc, thread_t thread, uint32_t *signal, nxu_sigaction_t *action)
{
	if (signal != 0) *signal = 0U;

	if (proc == 0 || thread == 0 || signal == 0 || action == 0) return SIGNAL_TAKE_NONE;

	for (;;) {
		signal_park_while_stopped(proc);

		uint32_t cont_seq;
		uint32_t taken = signal_dequeue_seq(proc, thread, &cont_seq);

		if (taken == 0U) return SIGNAL_TAKE_NONE;

		*signal = taken;
		(void)signal_get_action(proc, taken, action);

		if (taken == NXU_SIGKILL) return SIGNAL_TAKE_TERMINATE;

		if (taken != NXU_SIGSTOP) {
			if (action->handler == NXU_SIG_IGN) continue;

			if (action->handler != NXU_SIG_DFL) return SIGNAL_TAKE_HANDLER;
		}

		switch (signal_default_action(taken)) {
		case SIGNAL_DEFAULT_TERMINATE:
			return SIGNAL_TAKE_TERMINATE;
		case SIGNAL_DEFAULT_STOP:
			signal_stop_self(proc, taken, cont_seq);
			break;
		default:
			/* Ignored; or SIGCONT, whose work was done when it was sent. */
			break;
		}
	}
}

uint32_t
signal_blocked_mask_for_handler(uint32_t current_mask, uint32_t signal, const nxu_sigaction_t *action)
{
	uint32_t mask = current_mask;

	if (action != 0) {
		mask |= action->mask;
		if ((action->flags & NXU_SA_NODEFER) == 0U) mask |= SIGNAL_BIT(signal);
	}

	return signal_sanitize_mask(mask);
}

void
signal_notify_parent_of_exit_locked(proc_t parent)
{
	if (parent == 0) return;

	/* Called under the process table lock; p_siglock is a leaf below it. */
	uint64_t irq_state = nxu_spin_lock_irqsave(&parent->p_siglock);

	if (parent->p_sigact[NXU_SIGCHLD].handler > NXU_SIG_IGN) {
		parent->p_sigpending |= SIGNAL_BIT(NXU_SIGCHLD);
	}

	nxu_spin_unlock_irqrestore(&parent->p_siglock, irq_state);
}
