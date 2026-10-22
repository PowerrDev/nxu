/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/crash.c
 *
 * Captures process crash state and collects user thread backtraces.
 */
#include <kern/process/crash.h>
#include <kern/process/signal.h>
#include <kern/console/console.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/machine/timer.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/vmm.h>
#include <libk/string.h>

#if defined(__aarch64__)
#include <kern/arm64/exception.h>
#endif

#define CRASH_SLOTS 8U
#define SLOT_FREE 0U
#define SLOT_CAPTURING 1U
#define SLOT_READY 2U

static nxu_spinlock_t g_lock;
static waitq_t g_wait;

static struct {
	uint32_t state;
	nxu_crash_report_t report;
} g_slots[CRASH_SLOTS];

static uint64_t g_incident, g_dropped, g_consumer;

/* Never dereference user addresses directly or demand-fault pages during a crash.
 * Teardown owns this map and keeps it alive until all threads are collected. */
static bool
read_stack(vm_address_space_t *map, uint64_t address, void *out, uint32_t size)
{
	if (address > UINT64_MAX - size) return false;

	uint8_t *bytes = out;

	while (size != 0U) {
		vm_user_page_mapping_t mapping;
		uint64_t page = address & ~(PMM_PAGE_SIZE - 1ULL), kernel;
		uint32_t offset = address & (PMM_PAGE_SIZE - 1ULL);
		uint32_t chunk = (uint32_t)PMM_PAGE_SIZE - offset;

		if (chunk > size) chunk = size;

		if (!vm_address_space_query_page(map, page, &mapping) ||
			!vmm_physical_to_higher_half(mapping.physical_address, &kernel))
			return false;

		memcpy(bytes, (const void *)(uintptr_t)(kernel + offset), chunk);

		bytes += chunk;
		address += chunk;
		size -= chunk;
	}

  return true;
}

static void
capture(thread_t thread, const void *raw)
{
	nxu_crash_thread_t *out = &thread->crash_state;

  memset(out, 0, sizeof(*out));
	
  out->tid = thread_tid(thread);
#if defined(__aarch64__)
	const arm64_exception_frame_t *frame = raw;
	if (frame == 0) return;

	memcpy(out->registers, frame->x, sizeof(out->registers));

  out->pc = frame->elr;
	out->sp = frame->sp_el0;
	out->fp = frame->x[29];
	out->status = frame->spsr;
	out->fault_address = frame->far;
	out->syndrome = frame->esr;
	out->flags = NXU_CRASH_REGISTERS_VALID;
	out->frames[out->frame_count++] = out->pc;

#else
	(void)raw;

  /* The i386 port has no POSIX signal delivery or persistent user trap frame.
	 * Do not manufacture architectural state for that port. */
	(void)read_stack;
#endif
}

static void
backtrace(thread_t thread)
{
#if defined(__aarch64__)
	nxu_crash_thread_t *out = &thread->crash_state;

  if (!(out->flags & NXU_CRASH_REGISTERS_VALID)) return;

	out->frame_count = 1U;
	uint64_t fp = out->fp;

  while (fp != 0 && out->frame_count < NXU_CRASH_FRAMES) {
		uint64_t pair[2];

    if ((fp & 15ULL) != 0 || fp < out->sp || fp - out->sp > 8ULL * 1024ULL * 1024ULL ||
			!read_stack(&thread->home_task->map, fp, pair, sizeof(pair))) {
			out->flags |= NXU_CRASH_STACK_INCOMPLETE;
			break;
		}

		if (pair[1] == 0) break;

		vm_user_page_mapping_t code;
	
    /* A user entry's outer LR may point back to the kernel transition.
		 * Only executable user mappings can be caller frames. */
		if (!vm_address_space_query_page(&thread->home_task->map, (pair[1] - 4ULL) & ~(PMM_PAGE_SIZE - 1ULL), &code) ||
			code.protection != VM_USER_PROTECTION_READ_EXECUTE)
			break;
	
    out->frames[out->frame_count++] = pair[1];
	
    if (pair[0] == 0) {
			fp = 0;
			break;
		}

		if (pair[0] <= fp) {
			out->flags |= NXU_CRASH_STACK_INCOMPLETE;
			break;
		}

		fp = pair[0];
	}
	if (fp != 0 && out->frame_count == NXU_CRASH_FRAMES) out->flags |= NXU_CRASH_STACK_INCOMPLETE;
#else
	(void)thread;
#endif
}

void
crash_save_current(const void *frame)
{
	thread_t thread = current_thread();
	if (thread == 0 || thread->home_task == 0) return;

	proc_t proc = thread->home_task->bsd_info;
	if (proc != 0 && __atomic_load_n(&proc->p_crash_report, __ATOMIC_ACQUIRE) != 0) capture(thread, frame);
}

void
crash_begin(proc_t proc, uint64_t status)
{
	if (!NXU_EXIT_WAS_KILLED(status)) return;

	uint32_t signal = NXU_EXIT_TERMSIG(status);
#if defined(__aarch64__)
	switch (signal) {
    case NXU_SIGSEGV:
    case NXU_SIGBUS:
    case NXU_SIGILL:
    case NXU_SIGFPE:
    case NXU_SIGABRT:
    case NXU_SIGTRAP:
	  case NXU_SIGQUIT:
		break;
	
    default:
		  return; /* routine SIGTERM / SIGKILL are not crashes */
	}
#else
	return;
#endif
	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	nxu_crash_report_t *report = 0;
	
  for (uint32_t i = 0; i < CRASH_SLOTS; i++)
		if (g_slots[i].state == SLOT_FREE) {
			g_slots[i].state = SLOT_CAPTURING;
			report = &g_slots[i].report;

      break;
		}

	uint64_t incident = ++g_incident;
	if (report == 0) {
		g_dropped++;
    nxu_spin_unlock_irqrestore(&g_lock, irq);
	
    return;
	}

	/* Only this exiting process writes its reserved slot until publication. */
	nxu_spin_unlock_irqrestore(&g_lock, irq);
	memset(report, 0, sizeof(*report));
	
  report->version = NXU_CRASH_VERSION;
	report->struct_size = sizeof(*report);
	report->incident = incident;
	report->uptime_us = timer_get_microseconds();
	report->pid = proc->p_ident.pid;
	report->uniqueid = proc->p_ident.uniqueid;
	report->ppid = proc->p_pptr != 0 ? proc->p_pptr->p_ident.pid : 0U;
	report->capabilities = proc->p_caps;
	report->signal = signal;
	report->architecture = NXU_CRASH_ARM64;
	report->faulting_tid = current_proc() == proc ? thread_tid(current_thread()) : 0;

  memcpy(report->name, proc->p_comm, sizeof(report->name));
	memcpy(report->executable, proc->p_exec_path, sizeof(report->executable));
	memcpy(report->image_id, proc->p_image_id, sizeof(report->image_id));

  report->image_count = proc->p_dyrld_count;
	memcpy(report->images, proc->p_dyrld_images, proc->p_dyrld_count * sizeof(proc->p_dyrld_images[0]));

  if (proc->p_dyrld_main[0]) report->images[report->image_count++] = proc->p_dyrld_interpreter;

	__atomic_store_n(&proc->p_crash_report, report, __ATOMIC_RELEASE);
#if defined(__aarch64__)
	/* Preserve the original fault even if a sibling fails to leave its CPU.
	 * Collection after the rendezvous replaces this with its full backtrace. */
	if (current_proc() == proc) {
		thread_t thread = current_thread();
		capture(thread, thread->machine.user_frame);

    report->threads[0] = thread->crash_state;
		report->thread_count = 1U;
	}
#endif
}

void
crash_collect_thread(task_t task, thread_t thread)
{
	proc_t proc = task->bsd_info;
	nxu_crash_report_t *report = proc != 0 ? __atomic_load_n(&proc->p_crash_report, __ATOMIC_ACQUIRE) : 0;

  if (report == 0) return;

#if defined(__aarch64__)
	/* A blocked syscall retains its frame; an interrupted remote thread saved
	 * its frame before unwind. The current exiting thread still owns its frame. */
	if (thread == current_thread() || thread->machine.user_frame != 0) capture(thread, thread->machine.user_frame);
#endif
	backtrace(thread);
	for (uint32_t i = 0; i < report->thread_count; i++)
		if (report->threads[i].tid == thread_tid(thread)) {
			report->threads[i] = thread->crash_state;
	
      return;
		}

	if (report->thread_count == NXU_CRASH_THREADS) {
		report->flags |= NXU_CRASH_THREADS_TRUNCATED;
		
    return;
	}

	nxu_crash_thread_t *out = &report->threads[report->thread_count++];

  *out = thread->crash_state;
	out->tid = thread_tid(thread);

  if ((out->flags & NXU_CRASH_REGISTERS_VALID) == 0U) report->flags |= NXU_CRASH_SNAPSHOT_INCOMPLETE;
}

void
crash_incomplete(task_t task)
{
	proc_t proc = task->bsd_info;

  if (proc != 0 && proc->p_crash_report != 0) proc->p_crash_report->flags |= NXU_CRASH_SNAPSHOT_INCOMPLETE;
}

void
crash_finish(proc_t proc, bool complete)
{
	nxu_crash_report_t *report = __atomic_exchange_n(&proc->p_crash_report, 0, __ATOMIC_ACQ_REL);
  
  if (report == 0) return;

	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	
  for (uint32_t i = 0; i < CRASH_SLOTS; i++)
		if (&g_slots[i].report == report) {
			report->dropped = g_dropped;
			g_slots[i].state = complete ? SLOT_READY : SLOT_FREE;

      if (!complete) g_dropped++;
			break;
		}

	nxu_spin_unlock_irqrestore(&g_lock, irq);

  waitq_wake_all(&g_wait);
}

int64_t
crash_next(proc_t consumer, nxu_crash_report_t *out)
{
	for (;;) {
		uint32_t sequence = waitq_seq(&g_wait);
		uint64_t irq = nxu_spin_lock_irqsave(&g_lock);

    if (g_consumer != 0 && g_consumer != consumer->p_ident.uniqueid) {
			nxu_spin_unlock_irqrestore(&g_lock, irq);
	
      return -NXU_SYS_E_DENIED;
		}

		g_consumer = consumer->p_ident.uniqueid;
		uint32_t first = CRASH_SLOTS;
		
    for (uint32_t i = 0; i < CRASH_SLOTS; i++)
			if (g_slots[i].state == SLOT_READY &&
				(first == CRASH_SLOTS || g_slots[i].report.incident < g_slots[first].report.incident))
				first = i;
		if (first != CRASH_SLOTS) *out = g_slots[first].report;
	
    nxu_spin_unlock_irqrestore(&g_lock, irq);
		if (first != CRASH_SLOTS) return 0;

		if (signal_pending_for(consumer, current_thread()) || !waitq_block_seq(&g_wait, sequence, true))
			return -NXU_SYS_E_INTERRUPTED;
	}
}

int64_t
crash_ack(proc_t consumer, uint64_t incident)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	int64_t result = -NXU_SYS_E_NOT_FOUND;
	
  if (g_consumer != consumer->p_ident.uniqueid)
		result = -NXU_SYS_E_DENIED;
	else
    for (uint32_t i = 0; i < CRASH_SLOTS; i++)
			if (g_slots[i].state == SLOT_READY && g_slots[i].report.incident == incident) {
				g_slots[i].state = SLOT_FREE;
				result = 0;
	
        break;
			}

	nxu_spin_unlock_irqrestore(&g_lock, irq);

  return result;
}

void
crash_release(proc_t proc)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	if (g_consumer == proc->p_ident.uniqueid) g_consumer = 0;
	nxu_spin_unlock_irqrestore(&g_lock, irq);
	waitq_wake_all(&g_wait);
}

static waitq_t g_ui_wait;
static nxu_crash_notification_t g_notifications[CRASH_SLOTS];
static uint32_t g_ui_head, g_ui_count;

int64_t
crash_notify(const nxu_crash_notification_t *notification)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	
  if (g_ui_count == CRASH_SLOTS) {
		nxu_spin_unlock_irqrestore(&g_lock, irq);

    return -NXU_SYS_E_NO_SPACE;
	}

	g_notifications[(g_ui_head + g_ui_count) % CRASH_SLOTS] = *notification;
	g_ui_count++;
	
  nxu_spin_unlock_irqrestore(&g_lock, irq);
	waitq_wake_all(&g_ui_wait);

  return 0;
}

int64_t
crash_ui_next(nxu_crash_notification_t *notification)
{
	for (;;) {
		uint32_t sequence = waitq_seq(&g_ui_wait);
		uint64_t irq = nxu_spin_lock_irqsave(&g_lock);

    if (g_ui_count) {
			*notification = g_notifications[g_ui_head];
			g_ui_head = (g_ui_head + 1U) % CRASH_SLOTS;
			g_ui_count--;
	
      nxu_spin_unlock_irqrestore(&g_lock, irq);
		
      return 0;
		}

		nxu_spin_unlock_irqrestore(&g_lock, irq);
		
    if (signal_pending_for(current_proc(), current_thread()) || !waitq_block_seq(&g_ui_wait, sequence, true))
			return -NXU_SYS_E_INTERRUPTED;
	}
}
