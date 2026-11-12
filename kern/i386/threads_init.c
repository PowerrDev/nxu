/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/threads_init.c
 *
 * The `threads` boot phase (see boot_info.h): process manager, thread
 * subsystem and scheduler bootstrap, the same sequence kern/kern_init.c
 * runs on arm64. After it returns the boot context is the scheduler's
 * bootstrap thread and kernel threads can be created and started.
 *
 * Once the scheduler can take a tick, the phase enables interrupts if an
 * earlier phase has taken over the interrupt controller (the interrupts
 * phase leaves IF clear and expects this one to set it). A threads-only
 * bring-up, where the PIC is still at its BIOS vector base and IRQ0 would
 * land on the double-fault vector, leaves IF clear; the idle thread and
 * every kernel thread still enable it on their own first run.
 */

/* Present only when the interrupts area is linked in (kern/i386/pic.c). */
extern void pic_init(void) __attribute__((weak));

#include <kern/i386/boot_info.h>

#include <kern/machine/machine_routines.h>

#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>

bool
i386_init_threads(const i386_boot_info_t *boot)
{
	(void)boot;

	/*
	 * The `kernel` phase normally brings the process manager up first;
	 * proc_bootstrap() is idempotent, so a boot that has not run it (a
	 * threads-only bring-up) is not a failure.
	 */
	if (!proc_bootstrap() || proc_kernel() == 0) {
		kprintf("process manager unavailable\n");
		return false;
	}

	if (!thread_bootstrap()) {
		kprintf("thread_bootstrap failed\n");
		return false;
	}

	if (!sched_bootstrap(proc_task(proc_kernel()))) {
		kprintf("sched_bootstrap failed\n");
		return false;
	}

	if (!sched_validate() || !thread_validate()) {
		kprintf("bootstrap validation failed\n");
		return false;
	}

	kprintf("scheduler up, %u threads (bootstrap tid %llu, idle tid %llu)\n", thread_count(),
			(unsigned long long)thread_tid(sched_bootstrap_thread()),
			(unsigned long long)thread_tid(sched_idle_thread()));

	if (pic_init != 0) {
		ml_irq_enable();
		kprintf("interrupts enabled\n");
	} else {
		kprintf("no interrupt controller driver linked, interrupts left disabled\n");
	}

	sched_dump();
	thread_dump();
	return true;
}
