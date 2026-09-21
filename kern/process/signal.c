/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/signal.c
 *
 * See kern/process/signal.h.
 */

#include <kern/process/signal.h>

#include <kern/lock.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/machine/vm_param.h>

#include <stdbool.h>
#include <stdint.h>

#define SIGNAL_BIT(signal) (1U << (signal))

/*
 * Locking. Threads of one process run on different CPUs and send, catch and
 * dequeue its signals concurrently, so p_sigact and p_sigpending are read and
 * written only under proc->p_siglock (a leaf spinlock, taken with interrupts
 * masked; nothing is called while it is held). A struct copy of a disposition
 * would otherwise tear against sigaction() on another CPU. signal_pending_for()
 * is the one lock-free reader: a single atomic word, used only as a hint before
 * sleeping.
 */

bool signal_valid(uint32_t signal)
{
	return signal >= 1U && signal < NXU_NSIG;
}

signal_default_t signal_default_action(uint32_t signal)
{
	return signal == NXU_SIGCHLD
		? SIGNAL_DEFAULT_IGNORE
		: SIGNAL_DEFAULT_TERMINATE;
}

uint32_t signal_sanitize_mask(uint32_t mask)
{
	return mask & ~(1U | NXU_SIG_UNCATCHABLE_MASK);
}

bool signal_set_action(proc_t proc, uint32_t signal, const nxu_sigaction_t *action)
{
	if (
		proc == 0 ||
		action == 0 ||
		!signal_valid(signal) ||
		signal == NXU_SIGKILL ||
		signal == NXU_SIGSTOP ||
		signal == NXU_SIGCONT
	) {
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

bool signal_get_action(proc_t proc, uint32_t signal, nxu_sigaction_t *action)
{
	if (proc == 0 || action == 0 || !signal_valid(signal)) return false;

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	*action = proc->p_sigact[signal];

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);
	return true;
}

void signal_inherit(proc_t child, proc_t parent)
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

void signal_reset_for_exec(proc_t proc)
{
	if (proc == 0) return;

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	for (uint32_t signal = 0U; signal < NXU_NSIG; signal++) {
		if (proc->p_sigact[signal].handler > NXU_SIG_IGN) {
			proc->p_sigact[signal] = (nxu_sigaction_t) { 0 };
		}
	}

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);
}

bool signal_pending_for(proc_t proc, thread_t thread)
{
	if (proc == 0 || thread == 0) return false;

	return (__atomic_load_n(&proc->p_sigpending, __ATOMIC_ACQUIRE) & (~thread->sig_blocked | NXU_SIG_UNCATCHABLE_MASK)) != 0U;
}

/*
 * signal_interrupt_sleepers
 *
 * A signal just became pending on proc: cut short the interruptible sleep of
 * every thread that would take it, so its system call returns "interrupted"
 * and the handler can run instead of the signal waiting behind the sleep.
 */
static void signal_interrupt_sleepers(proc_t proc, uint32_t signal)
{
	/*
	 * Walk the threads with references, not through the raw list: a thread of
	 * this process can exit on another CPU at any moment and be reclaimed. Each
	 * one that would take the signal is also kicked if it is running, so a signal
	 * for a thread in user mode on another CPU is delivered at its next return
	 * to user mode (one interrupt away) rather than up to a timer tick later.
	 */
	for (
		thread_t thread = task_first_thread_ref(&proc->p_task);
		thread != 0;
	) {
		if ((thread->sig_blocked & SIGNAL_BIT(signal)) == 0U || signal == NXU_SIGKILL) {
			(void)waitq_interrupt(thread);
			sched_kick_thread(thread);
		}

		thread_t next = thread_next_task_thread_ref(thread);

		thread_deallocate(thread);
		thread = next;
	}
}

signal_send_result_t signal_send(proc_t sender, proc_id_t pid, uint32_t signal)
{
	if (sender == 0) return SIGNAL_SEND_INVALID;
	if (signal != 0U && !signal_valid(signal)) return SIGNAL_SEND_INVALID;

	if (signal == NXU_SIGSTOP || signal == NXU_SIGCONT) return SIGNAL_SEND_NOT_SUPPORTED;

	/* The kernel process is not a target. */
	if (pid == PROC_PID_KERNEL) return SIGNAL_SEND_DENIED;

	proc_t target = proc_find(pid);

	if (target == 0) return SIGNAL_SEND_NO_SUCH_PROCESS;

	signal_send_result_t result = SIGNAL_SEND_OK;

	if (target != sender && !proc_is_inferior(target, sender)) {
		result = SIGNAL_SEND_DENIED;
	} else if (signal != 0U) {
		nxu_sigaction_t action;

		(void)signal_get_action(target, signal, &action);

		bool terminate =
			signal == NXU_SIGKILL ||
			(action.handler == NXU_SIG_DFL && signal_default_action(signal) == SIGNAL_DEFAULT_TERMINATE);

		if (signal != NXU_SIGKILL && action.handler == NXU_SIG_IGN) {
			/* Ignored: nothing to record. */
		} else if (signal != NXU_SIGKILL && action.handler == NXU_SIG_DFL && !terminate) {
			/* Default-ignored (SIGCHLD): nothing to record. */
		} else if (terminate && target != sender) {
			/*
			 * Another process being killed: end it now (its threads may be
			 * running on other CPUs; proc_exit terminates them, waits until
			 * they are off their CPUs and reclaims everything), rather than wait
			 * for a return to user mode that a blocked thread might never make.
			 */
			(void)proc_exit(target, NXU_EXIT_KILLED_SIGNAL(signal));
		} else {
			/* Caught, or aimed at the sender itself: delivered on the next
			 * return to user mode. */
			uint64_t irq_state = nxu_spin_lock_irqsave(&target->p_siglock);

			target->p_sigpending |= SIGNAL_BIT(signal);

			nxu_spin_unlock_irqrestore(&target->p_siglock, irq_state);

			signal_interrupt_sleepers(target, signal);
		}
	}

	proc_rele(target);
	return result;
}

uint32_t signal_dequeue(proc_t proc, thread_t thread)
{
	if (proc == 0 || thread == 0) return 0U;

	if (!signal_pending_for(proc, thread)) return 0U;

	uint64_t irq_state = nxu_spin_lock_irqsave(&proc->p_siglock);

	uint32_t deliverable = proc->p_sigpending & (~thread->sig_blocked | NXU_SIG_UNCATCHABLE_MASK);
	uint32_t taken = 0U;

	for (uint32_t signal = 1U; signal < NXU_NSIG && deliverable != 0U; signal++) {
		if ((deliverable & SIGNAL_BIT(signal)) == 0U) continue;

		proc->p_sigpending &= ~SIGNAL_BIT(signal);
		taken = signal;
		break;
	}

	nxu_spin_unlock_irqrestore(&proc->p_siglock, irq_state);
	return taken;
}

uint32_t signal_blocked_mask_for_handler(uint32_t current_mask, uint32_t signal, const nxu_sigaction_t *action)
{
	uint32_t mask = current_mask;

	if (action != 0) {
		mask |= action->mask;
		if ((action->flags & NXU_SA_NODEFER) == 0U) mask |= SIGNAL_BIT(signal);
	}

	return signal_sanitize_mask(mask);
}

void signal_notify_parent_of_exit_locked(proc_t parent)
{
	if (parent == 0) return;

	/* Called under the process table lock; p_siglock is a leaf below it. */
	uint64_t irq_state = nxu_spin_lock_irqsave(&parent->p_siglock);

	if (parent->p_sigact[NXU_SIGCHLD].handler > NXU_SIG_IGN) {
		parent->p_sigpending |= SIGNAL_BIT(NXU_SIGCHLD);
	}

	nxu_spin_unlock_irqrestore(&parent->p_siglock, irq_state);
}
