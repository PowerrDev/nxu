#include <kern/loader/elf.h>
#include <mach/arm64/cache.h>
#include <kern/ipc/ipc_init.h>
#include <kern/ipc/ipc_port.h>
#include <kern/ipc/ipc_space.h>
#include <kern/process/task.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <vfs/vfs.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ELF_IDENT_SIZE 16U
#define ELF_CLASS_64 2U
#define ELF_DATA_LSB 1U
#define ELF_VERSION_CURRENT 1U
#define ELF_TYPE_EXEC 2U
#define ELF_MACHINE_AARCH64 183U
#define ELF_PT_LOAD 1U
#define ELF_PF_X 0x1U
#define ELF_PF_W 0x2U
#define ELF_PROGRAM_HEADER_MAX 16U
#define LOADER_USER_STACK_PAGES 4ULL
#define LOADER_USER_STACK_TOP 0x0000007FFFFFF000ULL
#define LOADER_USER_STACK_BASE (LOADER_USER_STACK_TOP - LOADER_USER_STACK_PAGES * PMM_PAGE_SIZE)

typedef struct {
	uint8_t ident[ELF_IDENT_SIZE];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t program_header_offset;
	uint64_t section_header_offset;
	uint32_t flags;
	uint16_t header_size;
	uint16_t program_header_entry_size;
	uint16_t program_header_count;
	uint16_t section_header_entry_size;
	uint16_t section_header_count;
	uint16_t section_header_string_index;
} elf64_header_t;

typedef struct {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t virtual_address;
	uint64_t physical_address;
	uint64_t file_size;
	uint64_t memory_size;
	uint64_t alignment;
} elf64_program_header_t;

_Static_assert(sizeof(elf64_header_t) == 64U, "ELF64 header layout mismatch");
_Static_assert(sizeof(elf64_program_header_t) == 56U, "ELF64 program header layout mismatch");

static bool
loader_add_overflow(uint64_t left, uint64_t right, uint64_t *result)
{
	if (result == 0 || left > UINT64_MAX - right) return true;
	*result = left + right;
	return false;
}

static bool
loader_align_up(uint64_t value, uint64_t alignment, uint64_t *result)
{
	if (result == 0 || alignment == 0ULL || (alignment & (alignment - 1ULL)) != 0ULL) return false;
	if (value > UINT64_MAX - (alignment - 1ULL)) return false;
	*result = (value + alignment - 1ULL) & ~(alignment - 1ULL);
	return true;
}

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
loader_header_valid(const elf64_header_t *header)
{
	if (header == 0) return false;
	if (header->ident[0] != 0x7FU || header->ident[1] != 'E' || header->ident[2] != 'L' || header->ident[3] != 'F') return false;
	if (header->ident[4] != ELF_CLASS_64 || header->ident[5] != ELF_DATA_LSB || header->ident[6] != ELF_VERSION_CURRENT) return false;
	if (header->type != ELF_TYPE_EXEC || header->machine != ELF_MACHINE_AARCH64 || header->version != ELF_VERSION_CURRENT) return false;
	if (header->header_size != sizeof(elf64_header_t) || header->program_header_entry_size != sizeof(elf64_program_header_t)) return false;
	if (header->program_header_count == 0U || header->program_header_count > ELF_PROGRAM_HEADER_MAX) return false;
	return header->entry != 0ULL;
}

static bool
loader_segment_protection(const elf64_program_header_t *segment, vm_user_protection_t *protection)
{
	if (segment == 0 || protection == 0) return false;
	if ((segment->flags & ELF_PF_W) != 0U && (segment->flags & ELF_PF_X) != 0U) return false;

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
loader_map_segment(vm_address_space_t *map, const elf64_program_header_t *segment)
{
	if (segment->memory_size < segment->file_size || segment->memory_size == 0ULL) return LOADER_STATUS_BAD_FORMAT;
	if ((segment->virtual_address & (PMM_PAGE_SIZE - 1ULL)) != (segment->offset & (PMM_PAGE_SIZE - 1ULL))) return LOADER_STATUS_BAD_FORMAT;

	uint64_t segment_end;
	if (loader_add_overflow(segment->virtual_address, segment->memory_size, &segment_end)) return LOADER_STATUS_BAD_FORMAT;
	if (segment->virtual_address < PMM_PAGE_SIZE || segment_end > LOADER_USER_STACK_BASE) return LOADER_STATUS_BAD_FORMAT;

	uint64_t page_start = segment->virtual_address & ~(PMM_PAGE_SIZE - 1ULL);
	uint64_t page_end;
	if (!loader_align_up(segment_end, PMM_PAGE_SIZE, &page_end)) return LOADER_STATUS_BAD_FORMAT;

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
loader_copy_segment(vnode_t vnode, vm_address_space_t *map, const elf64_program_header_t *segment)
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
	for (uint64_t address = LOADER_USER_STACK_BASE; address < LOADER_USER_STACK_TOP; address += PMM_PAGE_SIZE) {
		loader_status_t status = loader_map_zero_page(map, address, VM_USER_PROTECTION_READ_WRITE);
		if (status != LOADER_STATUS_OK) return status;
	}

	return LOADER_STATUS_OK;
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

	elf64_header_t header;
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

	elf64_program_header_t programs[ELF_PROGRAM_HEADER_MAX];
	status = loader_vnode_read_exact(vnode, header.program_header_offset, programs, program_table_size);
	if (status != LOADER_STATUS_OK) {
		vnode_rele(vnode);
		return status;
	}

	bool loadable = false;
	for (uint32_t index = 0U; index < header.program_header_count; index++) {
		elf64_program_header_t *segment = &programs[index];
		if (segment->type != ELF_PT_LOAD) continue;
		loadable = true;
		uint64_t file_end;
		if (loader_add_overflow(segment->offset, segment->file_size, &file_end) || file_end > vnode->v_size) {
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
	default: return "unknown";
	}
}
