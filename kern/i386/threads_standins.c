/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/threads_standins.c
 *
 * TEST-ONLY STAND-INS. Not part of the kernel.
 *
 * The thread/scheduler/syscall sources link against services other areas of
 * the i386 port own and are developing in parallel: the kernel heap and
 * vm_kern (vm), address spaces and user copies (vm), the VFS, block, input
 * and display drivers (devices), and the program loader (userland). So the
 * threads area can be built and booted alone, every such symbol is given a
 * WEAK definition here. Once the real service is linked in, its strong
 * definition wins and the stand-in is dead code; the integrator may then
 * drop this file by building with I386_THREADS_STANDINS=0 (see
 * makedefs/i386/threads.mk).
 *
 * Two kinds:
 *
 *   working    just enough of a heap, kernel-stack allocator, address-space
 *              lifecycle and user-copy (identity: paging is off in this
 *              worktree, so a user pointer is a kernel pointer) for the
 *              self-test to create threads and processes and run system
 *              calls that pass pointers;
 *
 *   inert      the VFS, drivers and loader, which only the system calls
 *              this area does not exercise reach; they fail cleanly.
 */

#include <kern/boot/boot_args.h>
#include <kern/boot/boot_mode.h>
#include <kern/console/display_owner.h>
#include <kern/loader/elf.h>
#include <kern/memory/heap.h>
#include <kern/process/thread.h>

#include <drivers/block/block_device.h>
#include <drivers/block/partition.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/mouse.h>
#include <drivers/video/display.h>
#include <drivers/virtio/virtio_input.h>
#include <vfs/vfs.h>
#include <vm/address_space.h>
#include <vm/user_copy.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_shm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STANDIN __attribute__((weak))

/* ---- working: kernel heap (a bump allocator that never frees) ----------- */

#define STANDIN_HEAP_SIZE (256U * 1024U)

static uint8_t g_standin_heap[STANDIN_HEAP_SIZE] __attribute__((aligned(16)));
static uint32_t g_standin_heap_used;

STANDIN void *kmalloc(size_t size)
{
	uint32_t rounded = ((uint32_t)size + 15U) & ~15U;

	if (size == 0U || rounded > STANDIN_HEAP_SIZE - g_standin_heap_used) return 0;

	void *block = &g_standin_heap[g_standin_heap_used];

	g_standin_heap_used += rounded;
	return block;
}

STANDIN void *kcalloc(size_t count, size_t size)
{
	void *block = kmalloc(count * size);

	if (block != 0) memset(block, 0, count * size);
	return block;
}

STANDIN bool kfree(void *address)
{
	(void)address;
	return true;
}

/* ---- working: kernel stacks (a fixed pool that does reclaim) ------------ */

#define STANDIN_STACK_COUNT 24U

static uint8_t g_standin_stacks[STANDIN_STACK_COUNT][THREAD_KERNEL_STACK_SIZE] __attribute__((aligned(4096)));
static bool g_standin_stack_used[STANDIN_STACK_COUNT];

STANDIN bool vm_kern_allocate(size_t size, vmm_protection_t protection, void **address)
{
	(void)protection;

	if (address == 0 || size != THREAD_KERNEL_STACK_SIZE) return false;

	for (uint32_t index = 0U; index < STANDIN_STACK_COUNT; index++) {
		if (g_standin_stack_used[index]) continue;

		g_standin_stack_used[index] = true;
		*address = g_standin_stacks[index];
		return true;
	}

	return false;
}

STANDIN bool vm_kern_free(void *address, size_t size)
{
	if (size != THREAD_KERNEL_STACK_SIZE) return false;

	for (uint32_t index = 0U; index < STANDIN_STACK_COUNT; index++) {
		if (address != (void *)g_standin_stacks[index]) continue;

		g_standin_stack_used[index] = false;
		return true;
	}

	return false;
}

/* ---- working: address spaces and user copies (no paging yet) ------------ */

STANDIN bool vm_address_space_create(vm_address_space_t *space)
{
	return space != 0;
}

STANDIN bool vm_address_space_activate(vm_address_space_t *space)
{
	return space != 0;
}

STANDIN bool vm_address_space_deactivate(void)
{
	return true;
}

STANDIN void vm_map_destroy_all(vm_address_space_t *space)
{
	(void)space;
}

/* With paging off, a user address is valid if it is below the kernel half. */
#define STANDIN_USER_LIMIT 0xC0000000ULL

static bool standin_user_range(uint64_t address, uint64_t size)
{
	return address != 0ULL && address < STANDIN_USER_LIMIT && size <= STANDIN_USER_LIMIT - address;
}

STANDIN bool vm_copy_from_user(void *destination, uint64_t user_address, uint64_t size)
{
	if (size == 0ULL) return true;
	if (!standin_user_range(user_address, size)) return false;

	memcpy(destination, (const void *)(uintptr_t)user_address, (size_t)size);
	return true;
}

STANDIN bool vm_copy_to_user(uint64_t user_address, const void *source, uint64_t size)
{
	if (size == 0ULL) return true;
	if (!standin_user_range(user_address, size)) return false;

	memcpy((void *)(uintptr_t)user_address, source, (size_t)size);
	return true;
}

STANDIN bool vm_copy_string_from_user(char *destination, uint64_t user_address, uint64_t capacity)
{
	if (capacity == 0ULL || !standin_user_range(user_address, 1ULL)) return false;

	const char *source = (const char *)(uintptr_t)user_address;

	for (uint64_t index = 0ULL; index < capacity; index++) {
		destination[index] = source[index];
		if (source[index] == '\0') return true;
	}

	return false;
}

/* ---- inert: VM maps and shared memory ----------------------------------- */

STANDIN bool vm_map_anon(vm_address_space_t *space, uint64_t size_bytes, vm_user_protection_t protection, uint64_t *out_va)
{
	(void)space;
	(void)size_bytes;
	(void)protection;
	(void)out_va;
	return false;
}

STANDIN bool vm_map_free(vm_address_space_t *space, uint64_t va, uint64_t size_bytes)
{
	(void)space;
	(void)va;
	(void)size_bytes;
	return false;
}

STANDIN bool vm_map_lookup(const vm_address_space_t *space, uint64_t va, uint64_t *out_start_va, uint64_t *out_end_va)
{
	(void)space;
	(void)va;
	(void)out_start_va;
	(void)out_end_va;
	return false;
}

STANDIN bool vm_shm_create(uint64_t size_bytes, vm_shm_region_t *regionp)
{
	(void)size_bytes;
	(void)regionp;
	return false;
}

STANDIN bool vm_shm_map_into(vm_address_space_t *space, uint64_t *next_va, vm_shm_region_t region, vm_user_protection_t protection, uint64_t *out_va)
{
	(void)space;
	(void)next_va;
	(void)region;
	(void)protection;
	(void)out_va;
	return false;
}

STANDIN bool vm_shm_reference(vm_shm_region_t region)
{
	(void)region;
	return false;
}

STANDIN void vm_shm_release(vm_shm_region_t region)
{
	(void)region;
}

/* ---- inert: VFS ---------------------------------------------------------- */

STANDIN void filedesc_init(filedesc_t filedesc)
{
	(void)filedesc;
}

STANDIN void filedesc_close_all(filedesc_t filedesc)
{
	(void)filedesc;
}

STANDIN vfs_status_t filedesc_get(filedesc_t filedesc, uint32_t descriptor, file_t *result)
{
	(void)filedesc;
	(void)descriptor;
	(void)result;
	return VFS_STATUS_BAD_FD;
}

STANDIN vfs_status_t filedesc_install(filedesc_t filedesc, file_t file, uint32_t *descriptor)
{
	(void)filedesc;
	(void)file;
	(void)descriptor;
	return VFS_STATUS_NO_SPACE;
}

STANDIN vfs_status_t file_alloc_socket(socket_t socket, file_t *result)
{
	(void)socket;
	(void)result;
	return VFS_STATUS_NOT_SUPPORTED;
}

STANDIN void file_rele(file_t file)
{
	(void)file;
}

STANDIN void vnode_rele(vnode_t vnode)
{
	(void)vnode;
}

STANDIN vfs_status_t vfs_close(filedesc_t filedesc, uint32_t descriptor)
{
	(void)filedesc;
	(void)descriptor;
	return VFS_STATUS_BAD_FD;
}

STANDIN vfs_status_t vfs_lookup(const char *path, vnode_t *result)
{
	(void)path;
	(void)result;
	return VFS_STATUS_NOT_FOUND;
}

STANDIN vfs_status_t vfs_mkdir(const char *path)
{
	(void)path;
	return VFS_STATUS_NOT_SUPPORTED;
}

STANDIN vfs_status_t vfs_mount(const char *filesystem_name, block_device_t device, const char *path)
{
	(void)filesystem_name;
	(void)device;
	(void)path;
	return VFS_STATUS_NOT_SUPPORTED;
}

STANDIN bool vfs_mount_get(uint32_t index, vfs_mount_info_t *info)
{
	(void)index;
	(void)info;
	return false;
}

STANDIN vfs_status_t vfs_open(filedesc_t filedesc, const char *path, uint32_t flags, uint32_t *descriptor)
{
	(void)filedesc;
	(void)path;
	(void)flags;
	(void)descriptor;
	return VFS_STATUS_NOT_FOUND;
}

STANDIN vfs_status_t vfs_read(filedesc_t filedesc, uint32_t descriptor, void *buffer, uint64_t size, uint64_t *read_size)
{
	(void)filedesc;
	(void)descriptor;
	(void)buffer;
	(void)size;
	(void)read_size;
	return VFS_STATUS_BAD_FD;
}

STANDIN vfs_status_t vfs_readdir(filedesc_t filedesc, uint32_t descriptor, vfs_dirent_t *entry)
{
	(void)filedesc;
	(void)descriptor;
	(void)entry;
	return VFS_STATUS_BAD_FD;
}

STANDIN vfs_status_t vfs_seek(filedesc_t filedesc, uint32_t descriptor, uint64_t offset)
{
	(void)filedesc;
	(void)descriptor;
	(void)offset;
	return VFS_STATUS_BAD_FD;
}

STANDIN vfs_status_t vfs_space_info(const char *path, vfs_space_info_t *info)
{
	(void)path;
	(void)info;
	return VFS_STATUS_NOT_FOUND;
}

STANDIN vfs_status_t vfs_sync_all(void)
{
	return VFS_STATUS_OK;
}

STANDIN vfs_status_t vfs_unlink(const char *path)
{
	(void)path;
	return VFS_STATUS_NOT_FOUND;
}

STANDIN vfs_status_t vfs_unmount(const char *path)
{
	(void)path;
	return VFS_STATUS_NOT_SUPPORTED;
}

STANDIN vfs_status_t vfs_write(filedesc_t filedesc, uint32_t descriptor, const void *buffer, uint64_t size, uint64_t *written_size)
{
	(void)filedesc;
	(void)descriptor;
	(void)buffer;
	(void)size;
	(void)written_size;
	return VFS_STATUS_BAD_FD;
}

/* ---- inert: boot arguments, loader, drivers ------------------------------ */

STANDIN const char *boot_args_raw(void)
{
	return "";
}

STANDIN bool boot_mode_is_triage_os(void)
{
	return false;
}

STANDIN loader_status_t loader_spawn(proc_t parent, const char *path, const char *name, proc_t *result)
{
	(void)parent;
	(void)path;
	(void)name;
	(void)result;
	return LOADER_STATUS_NOT_FOUND;
}

STANDIN loader_status_t loader_spawn_caps(proc_t parent, const char *path, const char *name, uint32_t caps, proc_t *result)
{
	(void)parent;
	(void)path;
	(void)name;
	(void)caps;
	(void)result;
	return LOADER_STATUS_NOT_FOUND;
}

STANDIN block_device_t block_device_get(uint32_t index)
{
	(void)index;
	return 0;
}

STANDIN bool block_device_verify(block_device_t device)
{
	(void)device;
	return false;
}

STANDIN bool block_layout_inspect(block_device_t device, block_layout_info_t *info)
{
	(void)device;
	(void)info;
	return false;
}

STANDIN bool block_partition_get(block_device_t device, uint32_t index, block_partition_info_t *info)
{
	(void)device;
	(void)index;
	(void)info;
	return false;
}

STANDIN bool display_owner_claim(proc_id_t pid)
{
	(void)pid;
	return false;
}

STANDIN bool display_owner_is(proc_id_t pid)
{
	(void)pid;
	return false;
}

STANDIN display_device_t *display_primary(void)
{
	return 0;
}

STANDIN bool display_present(display_device_t *display, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	(void)display;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	return false;
}

STANDIN bool input_read(input_event_t *event)
{
	(void)event;
	return false;
}

STANDIN uint32_t keyboard_modifiers(void)
{
	return 0U;
}

STANDIN uint32_t mouse_buttons(void)
{
	return 0U;
}

STANDIN int64_t mouse_x(void)
{
	return 0;
}

STANDIN int64_t mouse_y(void)
{
	return 0;
}

STANDIN void virtio_input_service(void)
{
}
