#include <kern/loader/elf.h>
#include <kern/loader/elf_format.h>
#include <mach/machine/cache.h>
#include <kern/ipc/ipc_init.h>
#include <kern/ipc/ipc_port.h>
#include <kern/ipc/ipc_space.h>
#include <kern/process/signal.h>
#include <kern/process/task.h>
#include <mach/machine/user.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <vfs/vfs.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/vm_fault.h>
#include <vm/vm_map.h>
#include <vm/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

_Static_assert(LOADER_PAGE_SIZE == PMM_PAGE_SIZE, "loader page size must match the physical page size");

/*
 * Where a check has a LOADER_ELF_WIDEN branch below, the widened (i386) path
 * calls the shared, self-tested helper from elf_format.h and the native path
 * keeps the original inline spelling: it is the same test, but the arm64
 * kernel image is compared byte for byte across this change, and re-spelling
 * the checks perturbs its code layout.
 */

static loader_status_t
loader_vnode_read_exact(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size)
{
	uint8_t *bytes = buffer;
	uint64_t complete = 0ULL;

	while (complete < size) {
		uint64_t read_size = 0ULL;
		vfs_status_t status = vnode_read(vnode, offset + complete, bytes + complete, size - complete, &read_size);
		if (status != VFS_STATUS_OK || read_size == 0ULL) return LOADER_STATUS_IO_ERROR;
		complete += read_size;
	}

	return LOADER_STATUS_OK;
}

static bool
loader_segment_protection(const loader_segment_t *segment, vm_user_protection_t *protection)
{
	if (segment == 0 || protection == 0) return false;
	if (!loader_segment_flags_valid(segment->flags)) return false;

	if ((segment->flags & ELF_PF_X) != 0U) {
		*protection = VM_USER_PROTECTION_READ_EXECUTE;
	} else if ((segment->flags & ELF_PF_W) != 0U) {
		*protection = VM_USER_PROTECTION_READ_WRITE;
	} else {
		*protection = VM_USER_PROTECTION_READ_ONLY;
	}

	return true;
}

static loader_status_t
loader_map_zero_page(vm_address_space_t *map, uint64_t virtual_address, vm_user_protection_t protection)
{
	uint64_t physical_address;
	if (!pmm_allocate_page(&physical_address)) return LOADER_STATUS_NO_MEMORY;

	uint64_t kernel_address;
	if (!vmm_physical_to_higher_half(physical_address, &kernel_address)) {
		(void)pmm_free_page(physical_address);
		return LOADER_STATUS_PROCESS_ERROR;
	}

	memset((void *)kernel_address, 0, PMM_PAGE_SIZE);

	if (!vm_address_space_map_page(map, virtual_address, physical_address, protection)) {
		(void)pmm_free_page(physical_address);
		return LOADER_STATUS_PROCESS_ERROR;
	}

	return LOADER_STATUS_OK;
}

static loader_status_t
loader_map_segment(vm_address_space_t *map, const loader_segment_t *segment)
{
#if LOADER_ELF_WIDEN
	if (!loader_segment_valid(segment)) return LOADER_STATUS_BAD_FORMAT;

	uint64_t segment_end = segment->virtual_address + segment->memory_size;
	uint64_t page_start = segment->virtual_address & ~(PMM_PAGE_SIZE - 1ULL);
	uint64_t page_end = (segment_end + PMM_PAGE_SIZE - 1ULL) & ~(PMM_PAGE_SIZE - 1ULL);
#else
	if (segment->memory_size < segment->file_size || segment->memory_size == 0ULL) return LOADER_STATUS_BAD_FORMAT;
	if ((segment->virtual_address & (PMM_PAGE_SIZE - 1ULL)) != (segment->offset & (PMM_PAGE_SIZE - 1ULL))) return LOADER_STATUS_BAD_FORMAT;

	uint64_t segment_end;
	if (loader_add_overflow(segment->virtual_address, segment->memory_size, &segment_end)) return LOADER_STATUS_BAD_FORMAT;
	if (segment->virtual_address < PMM_PAGE_SIZE || segment_end > LOADER_USER_STACK_BASE) return LOADER_STATUS_BAD_FORMAT;

	uint64_t page_start = segment->virtual_address & ~(PMM_PAGE_SIZE - 1ULL);
	uint64_t page_end;
	if (!loader_align_up(segment_end, PMM_PAGE_SIZE, &page_end)) return LOADER_STATUS_BAD_FORMAT;

#endif

	vm_user_protection_t protection;
	if (!loader_segment_protection(segment, &protection)) return LOADER_STATUS_BAD_FORMAT;

	for (uint64_t address = page_start; address < page_end; address += PMM_PAGE_SIZE) {
		vm_user_page_mapping_t existing;
		if (vm_address_space_query_page(map, address, &existing)) return LOADER_STATUS_BAD_FORMAT;

		loader_status_t status = loader_map_zero_page(map, address, protection);
		if (status != LOADER_STATUS_OK) return status;
	}

	return LOADER_STATUS_OK;
}

static loader_status_t
loader_copy_segment(vnode_t vnode, vm_address_space_t *map, const loader_segment_t *segment)
{
	uint64_t copied = 0ULL;

	while (copied < segment->file_size) {
		uint64_t address = segment->virtual_address + copied;
		uint64_t page_address = address & ~(PMM_PAGE_SIZE - 1ULL);
		uint64_t page_offset = address & (PMM_PAGE_SIZE - 1ULL);
		uint64_t chunk = PMM_PAGE_SIZE - page_offset;
		uint64_t remaining = segment->file_size - copied;
		if (chunk > remaining) chunk = remaining;

		vm_user_page_mapping_t mapping;
		if (!vm_address_space_query_page(map, page_address, &mapping)) return LOADER_STATUS_PROCESS_ERROR;

		uint64_t kernel_address;
		if (!vmm_physical_to_higher_half(mapping.physical_address, &kernel_address)) return LOADER_STATUS_PROCESS_ERROR;

		loader_status_t status = loader_vnode_read_exact(vnode, segment->offset + copied, (void *)(kernel_address + page_offset), chunk);
		if (status != LOADER_STATUS_OK) return status;
		copied += chunk;
	}

	if ((segment->flags & ELF_PF_X) != 0U) {
		uint64_t start_mapping = segment->virtual_address & ~(PMM_PAGE_SIZE - 1ULL);
		uint64_t end;
		if (loader_add_overflow(segment->virtual_address, segment->memory_size, &end)) return LOADER_STATUS_BAD_FORMAT;
		uint64_t page_end;
		if (!loader_align_up(end, PMM_PAGE_SIZE, &page_end)) return LOADER_STATUS_BAD_FORMAT;

		for (uint64_t address = start_mapping; address < page_end; address += PMM_PAGE_SIZE) {
			vm_user_page_mapping_t mapping;
			if (!vm_address_space_query_page(map, address, &mapping)) return LOADER_STATUS_PROCESS_ERROR;
			uint64_t kernel_address;
			if (!vmm_physical_to_higher_half(mapping.physical_address, &kernel_address)) return LOADER_STATUS_PROCESS_ERROR;
			cache_sync_instruction_range(kernel_address, PMM_PAGE_SIZE);
		}
	}

	return LOADER_STATUS_OK;
}

static loader_status_t
loader_map_stack(vm_address_space_t *map)
{
#if VM_DEMAND_PAGING
	/* Reserve only: the first push onto each page faults it in. */
	if (!vm_map_reserve(map, LOADER_USER_STACK_BASE, LOADER_USER_STACK_TOP, VM_USER_PROTECTION_READ_WRITE)) {
		return LOADER_STATUS_PROCESS_ERROR;
	}

	return LOADER_STATUS_OK;
#else
	for (uint64_t address = LOADER_USER_STACK_BASE; address < LOADER_USER_STACK_TOP; address += PMM_PAGE_SIZE) {
		loader_status_t status = loader_map_zero_page(map, address, VM_USER_PROTECTION_READ_WRITE);
		if (status != LOADER_STATUS_OK) return status;
	}

	return LOADER_STATUS_OK;
#endif
}

static loader_status_t
loader_start_process(proc_t proc)
{
	task_t task = proc_task(proc);
	thread_t thread = task_first_thread_ref(task);
	if (thread == 0) return LOADER_STATUS_PROCESS_ERROR;

	if (!thread_stack_alloc(thread)) {
		thread_deallocate(thread);
		return LOADER_STATUS_NO_MEMORY;
	}

	if (!proc_make_runnable(proc) || !proc_mark_running(proc)) {
		thread_deallocate(thread);
		return LOADER_STATUS_PROCESS_ERROR;
	}

	bool started = sched_thread_start(thread);
	thread_deallocate(thread);
	return started ? LOADER_STATUS_OK : LOADER_STATUS_PROCESS_ERROR;
}

/*
 * Every new process inherits a send right to the system bootstrap registry
 * (bootd's well-known port, see kern/ipc/ipc_init.h) at the process's own
 * name 1 -- see kern/ipc/ipc_space.c's ipc_space_init: a fresh space's
 * first insert always lands at name 1, so this happens before anything
 * else could claim it. A no-op before bootd calls
 * ipc_set_bootstrap_registry_port (bootd's own spawn included, and any
 * kernel-side test harness that never registers one), leaving
 * p_ipc_bootstrap_name at its IPC_SPACE_NAME_INVALID default.
 */
static void
loader_handoff_bootstrap_port(proc_t proc)
{
	ipc_port_t bootstrap = ipc_bootstrap_registry_port();
	if (bootstrap == IPC_PORT_NULL) return;
	if (!ipc_port_reference(bootstrap)) return;

	uint32_t name;
	if (ipc_space_insert_port(&proc->p_ipc, bootstrap, &name) != IPC_SUCCESS) {
		ipc_port_release(bootstrap);
		return;
	}

	proc->p_ipc_bootstrap_name = name;
}

static void
loader_discard_process(proc_t parent, proc_t proc)
{
	if (parent == 0 || proc == 0) return;
	proc_id_t pid = proc->p_ident.pid;
	(void)proc_exit(proc, 127ULL);
	uint64_t status;
	(void)proc_reap(parent, pid, &status);
}

loader_status_t
loader_images_equal(const char *left_path, const char *right_path, bool *equal)
{
	if (equal != 0) *equal = false;
	if (left_path == 0 || right_path == 0 || equal == 0) return LOADER_STATUS_INVALID;

	vnode_t left;
	vfs_status_t left_status = vfs_lookup(left_path, &left);
	if (left_status == VFS_STATUS_NOT_FOUND) return LOADER_STATUS_NOT_FOUND;
	if (left_status != VFS_STATUS_OK) return LOADER_STATUS_IO_ERROR;

	vnode_t right;
	vfs_status_t right_status = vfs_lookup(right_path, &right);
	if (right_status != VFS_STATUS_OK) {
		vnode_rele(left);
		return right_status == VFS_STATUS_NOT_FOUND ? LOADER_STATUS_NOT_FOUND : LOADER_STATUS_IO_ERROR;
	}

	if (left->v_type != VNODE_TYPE_REGULAR || right->v_type != VNODE_TYPE_REGULAR) {
		vnode_rele(right);
		vnode_rele(left);
		return LOADER_STATUS_BAD_FORMAT;
	}

	if (left->v_size != right->v_size) {
		vnode_rele(right);
		vnode_rele(left);
		*equal = false;
		return LOADER_STATUS_OK;
	}

	uint8_t left_buffer[256];
	uint8_t right_buffer[256];
	uint64_t offset = 0ULL;

	while (offset < left->v_size) {
		uint64_t chunk = left->v_size - offset;
		if (chunk > sizeof(left_buffer)) chunk = sizeof(left_buffer);

		loader_status_t status = loader_vnode_read_exact(left, offset, left_buffer, chunk);
		if (status == LOADER_STATUS_OK) status = loader_vnode_read_exact(right, offset, right_buffer, chunk);
		if (status != LOADER_STATUS_OK) {
			vnode_rele(right);
			vnode_rele(left);
			return status;
		}

		if (memcmp(left_buffer, right_buffer, chunk) != 0) {
			vnode_rele(right);
			vnode_rele(left);
			*equal = false;
			return LOADER_STATUS_OK;
		}

		offset += chunk;
	}

	vnode_rele(right);
	vnode_rele(left);
	*equal = true;
	return LOADER_STATUS_OK;
}

loader_status_t
loader_spawn(proc_t parent, const char *path, const char *name, proc_t *result)
{
	if (result != 0) *result = 0;
	if (parent == 0 || path == 0 || name == 0 || result == 0) return LOADER_STATUS_INVALID;

	vnode_t vnode;
	vfs_status_t lookup_status = vfs_lookup(path, &vnode);
	if (lookup_status == VFS_STATUS_NOT_FOUND) return LOADER_STATUS_NOT_FOUND;
	if (lookup_status != VFS_STATUS_OK) return LOADER_STATUS_IO_ERROR;
	if (vnode->v_type != VNODE_TYPE_REGULAR) {
		vnode_rele(vnode);
		return LOADER_STATUS_BAD_FORMAT;
	}

	loader_header_t header;
#if LOADER_ELF_WIDEN
	loader_disk_header_t disk_header;
	loader_status_t status = loader_vnode_read_exact(vnode, 0ULL, &disk_header, sizeof(disk_header));
	if (status == LOADER_STATUS_OK) loader_header_widen(&disk_header, &header);
#else
	loader_status_t status = loader_vnode_read_exact(vnode, 0ULL, &header, sizeof(header));
#endif
	if (status != LOADER_STATUS_OK || !loader_header_valid(&header)) {
		vnode_rele(vnode);
		return status == LOADER_STATUS_OK ? LOADER_STATUS_BAD_FORMAT : status;
	}

#if LOADER_ELF_WIDEN
	uint64_t program_table_size;
	if (!loader_program_table_valid(&header, vnode->v_size, &program_table_size)) {
#else
	uint64_t program_table_size = (uint64_t)header.program_header_count * sizeof(elf64_program_header_t);
	uint64_t program_table_end;
	if (loader_add_overflow(header.program_header_offset, program_table_size, &program_table_end) || program_table_end > vnode->v_size) {
#endif
		vnode_rele(vnode);
		return LOADER_STATUS_BAD_FORMAT;
	}

	loader_segment_t programs[ELF_PROGRAM_HEADER_MAX];
#if LOADER_ELF_WIDEN
	loader_disk_segment_t disk_programs[ELF_PROGRAM_HEADER_MAX];
	status = loader_vnode_read_exact(vnode, header.program_header_offset, disk_programs, program_table_size);
	for (uint32_t index = 0U; status == LOADER_STATUS_OK && index < header.program_header_count; index++) {
		loader_segment_widen(&disk_programs[index], &programs[index]);
	}
#else
	status = loader_vnode_read_exact(vnode, header.program_header_offset, programs, program_table_size);
#endif
	if (status != LOADER_STATUS_OK) {
		vnode_rele(vnode);
		return status;
	}

	bool loadable = false;
	for (uint32_t index = 0U; index < header.program_header_count; index++) {
		loader_segment_t *segment = &programs[index];
		if (segment->type != ELF_PT_LOAD) continue;
		loadable = true;
#if LOADER_ELF_WIDEN
		if (!loader_segment_in_file(segment, vnode->v_size)) {
#else
		uint64_t file_end;
		if (loader_add_overflow(segment->offset, segment->file_size, &file_end) || file_end > vnode->v_size) {
#endif
			vnode_rele(vnode);
			return LOADER_STATUS_BAD_FORMAT;
		}
	}

	if (!loadable) {
		vnode_rele(vnode);
		return LOADER_STATUS_BAD_FORMAT;
	}

	proc_t proc;
	if (!proc_create_user(parent, name, header.entry, LOADER_USER_STACK_TOP, &proc)) {
		vnode_rele(vnode);
		return LOADER_STATUS_PROCESS_ERROR;
	}

	loader_handoff_bootstrap_port(proc);

	vm_address_space_t *map = proc_vm_map(proc);
	if (map == 0) status = LOADER_STATUS_PROCESS_ERROR;

	for (uint32_t index = 0U; status == LOADER_STATUS_OK && index < header.program_header_count; index++) {
		if (programs[index].type != ELF_PT_LOAD) continue;
		status = loader_map_segment(map, &programs[index]);
	}

	for (uint32_t index = 0U; status == LOADER_STATUS_OK && index < header.program_header_count; index++) {
		if (programs[index].type != ELF_PT_LOAD) continue;
		status = loader_copy_segment(vnode, map, &programs[index]);
	}

	if (status == LOADER_STATUS_OK) status = loader_map_stack(map);

	if (status == LOADER_STATUS_OK) {
		vm_user_page_mapping_t entry_mapping;
		uint64_t entry_page = header.entry & ~(PMM_PAGE_SIZE - 1ULL);
		if (!vm_address_space_query_page(map, entry_page, &entry_mapping) || entry_mapping.protection != VM_USER_PROTECTION_READ_EXECUTE) status = LOADER_STATUS_BAD_FORMAT;
	}

	vnode_rele(vnode);

	if (status == LOADER_STATUS_OK) status = loader_start_process(proc);
	if (status != LOADER_STATUS_OK) {
		loader_discard_process(parent, proc);
		return status;
	}

	*result = proc;
	return LOADER_STATUS_OK;
}

#if !LOADER_ELF_WIDEN

/*
 * loader_stack_write
 *
 * Copy data onto the new program's stack. The stack is only reserved (see
 * loader_map_stack), so each page is first faulted in through the same
 * resolver the program's own first touch would use, then written through
 * the kernel's direct map -- the new space is not the active one.
 */
static loader_status_t
loader_stack_write(vm_address_space_t *map, uint64_t address, const void *data, uint64_t size)
{
	const uint8_t *bytes = data;
	uint64_t written = 0ULL;

	while (written < size) {
		uint64_t current = address + written;
		uint64_t page_address = current & ~(PMM_PAGE_SIZE - 1ULL);
		uint64_t page_offset = current & (PMM_PAGE_SIZE - 1ULL);
		uint64_t chunk = PMM_PAGE_SIZE - page_offset;
		if (chunk > size - written) chunk = size - written;

		vm_user_page_mapping_t mapping;
		if (!vm_address_space_query_page(map, page_address, &mapping)) {
			if (!vm_fault_user(map, page_address, VM_FAULT_WRITE)) return LOADER_STATUS_NO_MEMORY;
			if (!vm_address_space_query_page(map, page_address, &mapping)) return LOADER_STATUS_PROCESS_ERROR;
		}

		uint64_t kernel_address;
		if (!vmm_physical_to_higher_half(mapping.physical_address, &kernel_address)) return LOADER_STATUS_PROCESS_ERROR;

		memcpy((void *)(kernel_address + page_offset), bytes + written, chunk);
		written += chunk;
	}

	return LOADER_STATUS_OK;
}

/*
 * loader_build_stack_args
 *
 * Lay argv out at the top of the new stack and return the initial stack
 * pointer and the address of argv[0]:
 *
 *     top   +-----------------------+
 *           | argument strings      |
 *           +-----------------------+
 *           | argv[argc] = NULL     |
 *           | argv[argc-1] ...      |
 *           | argv[0]               |  <- argv (x1)
 *     sp -> +-----------------------+  (16-byte aligned)
 */
static loader_status_t
loader_build_stack_args(vm_address_space_t *map, const loader_exec_args_t *args, uint64_t *out_sp, uint64_t *out_argv)
{
	uint64_t strings_base = (LOADER_USER_STACK_TOP - args->length) & ~0xFULL;
	uint64_t array_size = ((uint64_t)args->argc + 1ULL) * sizeof(uint64_t);
	uint64_t array_base = (strings_base - array_size) & ~0xFULL;

	if (array_base < LOADER_USER_STACK_BASE) return LOADER_STATUS_BAD_FORMAT;

	uint64_t pointers[NXU_EXEC_ARGV_MAX + 1U];
	uint32_t offset = 0U;

	for (uint32_t index = 0U; index < args->argc; index++) {
		pointers[index] = strings_base + offset;

		while (offset < args->length && args->strings[offset] != '\0') offset++;
		offset++;
	}

	pointers[args->argc] = 0ULL;

	loader_status_t status = loader_stack_write(map, strings_base, args->strings, args->length);
	if (status != LOADER_STATUS_OK) return status;

	status = loader_stack_write(map, array_base, pointers, array_size);
	if (status != LOADER_STATUS_OK) return status;

	*out_sp = array_base;
	*out_argv = array_base;
	return LOADER_STATUS_OK;
}

static void
loader_discard_space(vm_address_space_t *space)
{
	vm_map_destroy_all(space);
	(void)vm_address_space_release_pages(space);
	(void)vm_address_space_destroy(space);
}

loader_status_t
loader_exec(proc_t proc, const char *path, const loader_exec_args_t *args)
{
	if (proc == 0 || path == 0 || args == 0) return LOADER_STATUS_INVALID;
	if (args->argc == 0U || args->argc > NXU_EXEC_ARGV_MAX || args->length > NXU_EXEC_ARGV_BYTES) return LOADER_STATUS_INVALID;

	task_t task = proc_task(proc);
	if (task == 0 || task_is_kernel(task) || !task_is_active(task)) return LOADER_STATUS_INVALID;

	/* Replacing the image under a sibling thread is not supported. */
	if (task->thread_count != 1U) return LOADER_STATUS_BUSY;

	vnode_t vnode;
	vfs_status_t lookup_status = vfs_lookup(path, &vnode);
	if (lookup_status == VFS_STATUS_NOT_FOUND) return LOADER_STATUS_NOT_FOUND;
	if (lookup_status != VFS_STATUS_OK) return LOADER_STATUS_IO_ERROR;
	if (vnode->v_type != VNODE_TYPE_REGULAR) {
		vnode_rele(vnode);
		return LOADER_STATUS_BAD_FORMAT;
	}

	loader_header_t header;
	loader_status_t status = loader_vnode_read_exact(vnode, 0ULL, &header, sizeof(header));
	if (status != LOADER_STATUS_OK || !loader_header_valid(&header)) {
		vnode_rele(vnode);
		return status == LOADER_STATUS_OK ? LOADER_STATUS_BAD_FORMAT : status;
	}

	uint64_t program_table_size = (uint64_t)header.program_header_count * sizeof(elf64_program_header_t);
	uint64_t program_table_end;
	if (loader_add_overflow(header.program_header_offset, program_table_size, &program_table_end) || program_table_end > vnode->v_size) {
		vnode_rele(vnode);
		return LOADER_STATUS_BAD_FORMAT;
	}

	loader_segment_t programs[ELF_PROGRAM_HEADER_MAX];
	status = loader_vnode_read_exact(vnode, header.program_header_offset, programs, program_table_size);
	if (status != LOADER_STATUS_OK) {
		vnode_rele(vnode);
		return status;
	}

	bool loadable = false;
	for (uint32_t index = 0U; index < header.program_header_count; index++) {
		if (programs[index].type != ELF_PT_LOAD) continue;
		loadable = true;

		uint64_t file_end;
		if (loader_add_overflow(programs[index].offset, programs[index].file_size, &file_end) || file_end > vnode->v_size) {
			vnode_rele(vnode);
			return LOADER_STATUS_BAD_FORMAT;
		}
	}

	if (!loadable) {
		vnode_rele(vnode);
		return LOADER_STATUS_BAD_FORMAT;
	}

	/* Build the whole new image off to the side. */
	vm_address_space_t staging = { 0 };
	if (!vm_address_space_create(&staging)) {
		vnode_rele(vnode);
		return LOADER_STATUS_NO_MEMORY;
	}

	for (uint32_t index = 0U; status == LOADER_STATUS_OK && index < header.program_header_count; index++) {
		if (programs[index].type != ELF_PT_LOAD) continue;
		status = loader_map_segment(&staging, &programs[index]);
	}

	for (uint32_t index = 0U; status == LOADER_STATUS_OK && index < header.program_header_count; index++) {
		if (programs[index].type != ELF_PT_LOAD) continue;
		status = loader_copy_segment(vnode, &staging, &programs[index]);
	}

	if (status == LOADER_STATUS_OK) status = loader_map_stack(&staging);

	if (status == LOADER_STATUS_OK) {
		vm_user_page_mapping_t entry_mapping;
		uint64_t entry_page = header.entry & ~(PMM_PAGE_SIZE - 1ULL);
		if (!vm_address_space_query_page(&staging, entry_page, &entry_mapping) || entry_mapping.protection != VM_USER_PROTECTION_READ_EXECUTE) status = LOADER_STATUS_BAD_FORMAT;
	}

	vnode_rele(vnode);

	uint64_t stack_pointer = 0ULL;
	uint64_t argv_address = 0ULL;

	if (status == LOADER_STATUS_OK) status = loader_build_stack_args(&staging, args, &stack_pointer, &argv_address);

	/* Last thing that can fail: nothing of the old image has been touched. */
	if (status == LOADER_STATUS_OK && !machine_user_exec_state(header.entry, stack_pointer, args->argc, argv_address)) {
		status = LOADER_STATUS_PROCESS_ERROR;
	}

	if (status != LOADER_STATUS_OK) {
		loader_discard_space(&staging);
		return status;
	}

	/*
	 * Commit. Detach the old space (deactivating syncs its table
	 * accounting), move the new one into the task and make it the live one,
	 * then discard the old image.
	 */
	(void)vm_address_space_deactivate();

	vm_address_space_t old = task->map;

	task->map = staging;
	task->shm_next_va = VM_SHM_BASE;

	if (!vm_address_space_activate(&task->map)) {
		/* Nothing sensible is left to run: end the process. */
		loader_discard_space(&old);
		(void)proc_exit(proc, NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV));
		return LOADER_STATUS_PROCESS_ERROR;
	}

	loader_discard_space(&old);

	signal_reset_for_exec(proc);

	const char *name = path;
	for (const char *cursor = path; *cursor != '\0'; cursor++) {
		if (*cursor == '/' && cursor[1] != '\0') name = cursor + 1;
	}
	(void)proc_set_name(proc, name);

	return LOADER_STATUS_OK;
}

#endif /* !LOADER_ELF_WIDEN */

const char *
loader_status_name(loader_status_t status)
{
	switch (status) {
	case LOADER_STATUS_OK: return "ok";
	case LOADER_STATUS_INVALID: return "invalid";
	case LOADER_STATUS_NOT_FOUND: return "not found";
	case LOADER_STATUS_BAD_FORMAT: return "bad format";
	case LOADER_STATUS_NO_MEMORY: return "no memory";
	case LOADER_STATUS_IO_ERROR: return "I/O error";
	case LOADER_STATUS_PROCESS_ERROR: return "process error";
	case LOADER_STATUS_BUSY: return "busy";
	default: return "unknown";
	}
}
