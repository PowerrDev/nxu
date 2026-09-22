/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/post.c
 *
 * See kern/tests/post.h.
 */

#include <kern/tests/post.h>

#include <kern/console/console.h>
#include <kern/ipc/ipc_types.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/tests/ipc_test.h>
#include <kern/tests/vm_map_test.h>
#include <kern/tests/vm_shm_test.h>
#include <kern/arm64/exception.h>
#include <vfs/vfs.h>
#include <vm/pmm.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>

#include <crc32c.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Kernel virtual arena tests. */
#define KERNEL_VM_KERN_TEST_A_SIZE (PMM_PAGE_SIZE * 2ULL + 128ULL)
#define KERNEL_VM_KERN_TEST_B_SIZE (PMM_PAGE_SIZE + 256ULL)
#define KERNEL_VM_KERN_TEST_VALUE 0x564D4B45524E0000ULL

/* Kernel heap tests. */
#define KERNEL_HEAP_LARGE_TEST_SIZE (PMM_PAGE_SIZE * 3ULL + 333ULL)
#define KERNEL_HEAP_LARGE_TEST_VALUE 0xA5U

static volatile uint32_t g_sched_context_test_stage;

/*
 * post_sched_context_test_continue
 *
 * Run once on a freshly constructed kernel-thread stack, then voluntarily
 * yield back to the bootstrap thread. The bootstrap side terminates and
 * reaps this thread while it is queued, validating both directions of the
 * AArch64 context switch before userspace is dispatched.
 */
static void post_sched_context_test_continue(void *parameter)
{
	volatile uint32_t *stage = (volatile uint32_t *)parameter;

	*stage = 1U;

	if (!sched_yield()) {
		*stage = UINT32_MAX;
		return;
	}

	*stage = 2U;
}

static const char g_vfs_test_message[] = "NXU VFS ramfs self-test\n";

/*
 * post_test_vfs:
 *
 * Validate pathname traversal, vnode creation, per-process descriptors and
 * file offsets against the bootstrap ramfs root.
 */
static bool post_test_vfs(void)
{
	proc_t kernel_proc = proc_kernel();
	if (kernel_proc == 0) return false;

	filedesc_t filedesc = &kernel_proc->p_fd;

	if (vfs_mkdir("/tmp") != VFS_STATUS_OK) return false;

	uint32_t descriptor;
	vfs_status_t status = vfs_open(
		filedesc,
		"/tmp/hello",
		VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;

	uint64_t message_size = sizeof(g_vfs_test_message) - 1ULL;
	uint64_t written_size;
	status = vfs_write(filedesc, descriptor, g_vfs_test_message, message_size, &written_size);
	if (status != VFS_STATUS_OK || written_size != message_size) goto fail_close;

	if (vfs_seek(filedesc, descriptor, 0ULL) != VFS_STATUS_OK) goto fail_close;

	char buffer[sizeof(g_vfs_test_message)];
	memset(buffer, 0, sizeof(buffer));

	uint64_t read_size;
	status = vfs_read(filedesc, descriptor, buffer, message_size, &read_size);
	if (status != VFS_STATUS_OK || read_size != message_size) goto fail_close;

	for (uint64_t index = 0ULL; index < message_size; index++) {
		if (buffer[index] != g_vfs_test_message[index]) goto fail_close;
	}

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	vnode_t vnode;
	status = vfs_lookup("/tmp/./../tmp/hello", &vnode);
	if (status != VFS_STATUS_OK) return false;

	bool vnode_valid = vnode->v_type == VNODE_TYPE_REGULAR && vnode->v_size == message_size;
	vnode_rele(vnode);
	if (!vnode_valid) return false;

	status = vfs_read(filedesc, descriptor, buffer, sizeof(buffer), &read_size);
	if (status != VFS_STATUS_BAD_FD) return false;

	return filedesc->fd_open_count == 0U;

fail_close:
	(void)vfs_close(filedesc, descriptor);
	return false;
}

/*
 * post_test_crc32c:
 *
 * Verify the software Castagnoli implementation against the canonical
 * 123456789 check value using the running-checksum convention used by ext4.
 */
static bool post_test_crc32c(void)
{
	static const char vector[] = "123456789";
	return crc32c(~0U, vector, sizeof(vector) - 1U) == 0x1CF96D7CU;
}

static bool post_test_vm_kern(void)
{
	void *allocation_a = 0;
	void *allocation_b = 0;
	void *reused_allocation = 0;

	bool allocation_a_active = false;
	bool allocation_b_active = false;
	bool reused_allocation_active = false;

	bool passed = false;
	bool cleanup_passed = true;

	if (!vm_kern_allocate(
		KERNEL_VM_KERN_TEST_A_SIZE,
		VMM_PROTECTION_READ_WRITE,
		&allocation_a
	)) {
		goto cleanup;
	}

	allocation_a_active = true;

	for (uint64_t index = 0ULL; index < 3ULL; index++) {
		uint64_t virtual_address =
			(uint64_t)allocation_a +
			index * PMM_PAGE_SIZE;

		vmm_page_mapping_t mapping;

		if (!vmm_query_page(
			virtual_address,
			&mapping
		)) {
			goto cleanup;
		}

		if (
			mapping.memory_type != VMM_MEMORY_NORMAL ||
			mapping.protection !=
			VMM_PROTECTION_READ_WRITE
		) {
			goto cleanup;
		}

		uint64_t value = KERNEL_VM_KERN_TEST_VALUE + index;

		uint64_t direct_map_address;

		if (!vmm_physical_to_higher_half(
			mapping.physical_address,
			&direct_map_address
		)) {
			goto cleanup;
		}

		volatile uint64_t *virtual_word = (volatile uint64_t *)virtual_address;

		volatile uint64_t *physical_word = (volatile uint64_t *)direct_map_address;

		*virtual_word = value;

		if (*physical_word != value) {
			goto cleanup;
		}
	}

	if (!vm_kern_allocate(
		KERNEL_VM_KERN_TEST_B_SIZE,
		VMM_PROTECTION_READ_WRITE,
		&allocation_b
	)) {
		goto cleanup;
	}

	allocation_b_active = true;

	if (!vm_kern_free(
		allocation_a,
		KERNEL_VM_KERN_TEST_A_SIZE
	)) {
		goto cleanup;
	}

	allocation_a_active = false;

	if (!vm_kern_allocate(
		KERNEL_VM_KERN_TEST_A_SIZE,
		VMM_PROTECTION_READ_WRITE,
		&reused_allocation
	)) {
		goto cleanup;
	}

	reused_allocation_active = true;

	if (reused_allocation != allocation_a) {
		goto cleanup;
	}

	passed = true;

cleanup:
	if (
		allocation_a_active &&
		!vm_kern_free(
			allocation_a,
			KERNEL_VM_KERN_TEST_A_SIZE
		)
	) {
		cleanup_passed = false;
	}

	if (
		allocation_b_active &&
		!vm_kern_free(
			allocation_b,
			KERNEL_VM_KERN_TEST_B_SIZE
		)
	) {
		cleanup_passed = false;
	}

	if (
		reused_allocation_active &&
		!vm_kern_free(
			reused_allocation,
			KERNEL_VM_KERN_TEST_A_SIZE
		)
	) {
		cleanup_passed = false;
	}

	return passed && cleanup_passed;
}

static bool post_test_small_heap(void)
{
	void *allocation_a = kmalloc(24U);
	void *allocation_b = kmalloc(200U);
	void *allocation_c = kcalloc(32U, 8U);

	if (
		allocation_a == 0 ||
		allocation_b == 0 ||
		allocation_c == 0
	) {
		return false;
	}

	kverbosef("post_test_small_heap: allocation A at %p\n", allocation_a);
	kverbosef("post_test_small_heap: allocation B at %p\n", allocation_b);
	kverbosef("post_test_small_heap: allocation C at %p\n", allocation_c);

	if (!kfree(allocation_b)) {
		return false;
	}

	void *reused_allocation = kmalloc(128U);

	if (
		reused_allocation == 0 ||
		reused_allocation != allocation_b
	) {
		return false;
	}

	if (
		!kfree(allocation_a) ||
		!kfree(allocation_c) ||
		!kfree(reused_allocation)
	) {
		return false;
	}

	return true;
}

static bool post_test_large_heap(void)
{
	void *large_allocation = kmalloc(KERNEL_HEAP_LARGE_TEST_SIZE);

	if (
		large_allocation == 0 ||
		!vm_kern_contains(large_allocation)
	) {
		return false;
	}

	uint8_t *large_bytes = (uint8_t *)large_allocation;

	uint64_t page_count =
		(KERNEL_HEAP_LARGE_TEST_SIZE +
		PMM_PAGE_SIZE - 1ULL) /
		PMM_PAGE_SIZE;

	for (
		uint64_t page = 0ULL;
		page < page_count;
		page++
	) {
		uint64_t first_offset = page * PMM_PAGE_SIZE;

		uint64_t last_offset = first_offset + PMM_PAGE_SIZE - 1ULL;

		if (
			first_offset <
			KERNEL_HEAP_LARGE_TEST_SIZE
		) {
			large_bytes[first_offset] =
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page
				);
		}

		if (
			last_offset <
			KERNEL_HEAP_LARGE_TEST_SIZE
		) {
			large_bytes[last_offset] =
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page +
					16ULL
				);
		}
	}

	for (
		uint64_t page = 0ULL;
		page < page_count;
		page++
	) {
		uint64_t first_offset = page * PMM_PAGE_SIZE;

		uint64_t last_offset = first_offset + PMM_PAGE_SIZE - 1ULL;

		if (
			first_offset <
				KERNEL_HEAP_LARGE_TEST_SIZE &&
			large_bytes[first_offset] !=
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page
				)
		) {
			return false;
		}

		if (
			last_offset <
				KERNEL_HEAP_LARGE_TEST_SIZE &&
			large_bytes[last_offset] !=
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page +
					16ULL
				)
		) {
			return false;
		}
	}

	kverbosef("post_test_large_heap: large allocation at %p\n", large_allocation);

	heap_dump();

	if (!kfree(large_allocation)) {
		return false;
	}

	return !kfree(large_allocation);
}

bool post_storage(void);

/*
 * post_fail
 *
 * Log which check failed and report it to kernel_do_post()'s caller.
 */
static bool post_fail(const char *message)
{
	kputln(message);
	return false;
}

static bool post_memory(void)
{
	kputln("post_test_vm_kern: testing virtual allocations");

	if (!post_test_vm_kern()) {
		return post_fail("post_test_vm_kern: allocation test failed");
	}

	kputln("post_test_vm_kern: allocation test passed");
	vm_kern_dump();

	kputln("post_test_small_heap: testing allocation, reuse and free");

	if (!post_test_small_heap()) {
		return post_fail("post_test_small_heap: allocation, reuse or free test failed");
	}

	kputln("post_test_small_heap: allocation, reuse and free tests passed");
	heap_dump();

	kputln("post_test_large_heap: testing multi-page allocation");

	if (!post_test_large_heap()) {
		return post_fail("post_test_large_heap: multi-page allocation/free test failed");
	}

	kputln("post_test_large_heap: multi-page allocation/free test passed");
	heap_dump();

	return true;
}

static bool post_core(void)
{
	/* NXPC kernel message transport, shared memory and mmap. */

	if (!ipc_self_test()) {
		return post_fail(NXPC_LOG_PREFIX "kernel transport self-test failed");
	}

	if (!ipc_space_self_test()) {
		return post_fail(NXPC_LOG_PREFIX "per-process name table self-test failed");
	}

	kputln(NXPC_LOG_PREFIX "kernel transport ready");

	if (!vm_shm_self_test()) {
		return post_fail("vm_shm: self-test failed");
	}

	if (!shm_registry_self_test()) {
		return post_fail("shm_registry: self-test failed");
	}

	if (!vm_map_self_test()) {
		return post_fail("vm_map: self-test failed");
	}

	/* Checksums. */

	if (!post_test_crc32c()) {
		return post_fail("NXU_CRC32C_Checksum: self-test failed");
	}

	kputln("NXU_CRC32C_Checksum: self-test passed");

	/* Virtual filesystem. */

	if (!post_test_vfs()) {
		return post_fail("IOVirtualFSDriver pathname/file-descriptor self-test failed");
	}

	kputln("IOVirtualFSDriver pathname/file-descriptor self-test passed");
	vfs_dump();

	/* Scheduler. */

	if (!sched_run_queue_self_test()) {
		return post_fail("sched_run_queue_self_test: run queue self-test failed");
	}

	if (!sched_mlfq_self_test()) {
		return post_fail("sched_mlfq_self_test: MLFQ feedback self-test failed");
	}

	if (!sched_validate() || !thread_validate()) {
		return post_fail("sched_validate: bootstrap validation failed");
	}

	kputln("sched_validate: fixed-priority run queue self-test passed");
	kputln("sched_validate: MLFQ feedback self-test passed");
	sched_dump();
	thread_dump();

	/* AArch64 scheduler context-switch validation. */

	kputln("kernel_thread_create: testing AArch64 context switching");

	g_sched_context_test_stage = 0U;
	thread_t context_test_thread;

	if (!kernel_thread_create(
		proc_task(proc_kernel()),
		post_sched_context_test_continue,
		(void *)&g_sched_context_test_stage,
		&context_test_thread
	)) {
		return post_fail("kernel_thread_create: context-switch test thread creation failed");
	}

	if (!sched_thread_start(context_test_thread)) {
		return post_fail("sched_thread_start: context-switch test thread start failed");
	}

	if (!sched_yield()) {
		return post_fail("sched_yield: context-switch test dispatch failed");
	}

	if (
		g_sched_context_test_stage != 1U ||
		current_thread() != sched_bootstrap_thread()
	) {
		return post_fail("sched_bootstrap_thread: AArch64 context-switch test failed");
	}

	if (!sched_thread_terminate(context_test_thread)) {
		return post_fail("sched_thread_terminate: context-switch test termination failed");
	}

	if (!thread_reap(context_test_thread)) {
		return post_fail("thread_reap: context-switch test reap failed");
	}

	thread_deallocate(context_test_thread);

	if (!sched_validate() || !thread_validate()) {
		return post_fail("sched_validate: post-switch validation failed");
	}

	kputln("sched_validate: AArch64 context-switch test passed");

	/* EL0 exception-vector classification. */

	kputln("ARM64ExceptionHandler(): validating EL0 vector classification");

	if (!exception_validate_vector_classification()) {
		return post_fail("ARM64ExceptionHandler(): EL0 vector classification failed");
	}

	kputln("ARM64ExceptionHandler(): current EL synchronous vector classified");
	kputln("ARM64ExceptionHandler(): current EL IRQ vector classified");
	kputln("ARM64ExceptionHandler(): EL0 synchronous vector classified");
	kputln("ARM64ExceptionHandler(): EL0 IRQ vector classified");
	kputln("ARM64ExceptionHandler(): EL0 return state is recoverable");

	return true;
}

bool kernel_do_post(kernel_post_stage_t stage)
{
	switch (stage) {
	case KERNEL_POST_MEMORY:
		return post_memory();

	case KERNEL_POST_CORE:
		return post_core();

	case KERNEL_POST_STORAGE:
		return post_storage();
	}

	return post_fail("kernel_do_post: unknown stage");
}
