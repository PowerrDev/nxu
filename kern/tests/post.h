/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/post.h
 *
 * Kernel power-on self-test (POST). One entry runs every check the kernel
 * makes of itself -- checksums, allocators, IPC, VM, VFS, scheduler,
 * storage -- instead of scattering them through the boot sequence.
 *
 * The suite is staged because some checks are only meaningful at a
 * particular moment: the allocator tests assert exact address reuse, so
 * they must run on a fresh heap before any driver has allocated from it,
 * and the storage tests need /disk mounted. Boot calls kernel_do_post()
 * once as each stage's prerequisites come up.
 */

#ifndef NXU_KERN_TESTS_POST_H
#define NXU_KERN_TESTS_POST_H

#include <stdbool.h>

typedef enum {
	/* Kernel arena and heap. Needs vm_kern_init() and heap_init() only. */
	KERNEL_POST_MEMORY,

	/*
	 * Checksums, NXPC, shared memory and mmap, VFS, scheduler and the
	 * AArch64 context switch. Needs IPC, process manager, VFS and the
	 * scheduler bootstrapped.
	 */
	KERNEL_POST_CORE,

	/* Raw block read and the writable ext4 test. Needs /disk mounted. */
	KERNEL_POST_STORAGE
} kernel_post_stage_t;

/*
 * kernel_do_post
 *
 * Run one stage of the self-test suite. Logs what it checks and returns
 * false, after logging which check failed, if any check did; boot cannot
 * continue after a failed stage.
 */
bool kernel_do_post(kernel_post_stage_t stage);

#endif
