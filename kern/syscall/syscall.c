#include <kern/syscall/syscall.h>

#include <arch/arm64/timer.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/boot_mode.h>
#include <drivers/block/block_device.h>
#include <drivers/block/partition.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/mouse.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/video/display.h>
#include <kern/console/console.h>
#include <kern/exec/elf.h>
#include <kern/logging/version.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <vfs/vfs.h>
#include <vm/user_copy.h>

#include <stdint.h>
#include <string.h>

#define SYSCALL_STDOUT 1ULL
#define SYSCALL_STDERR 2ULL
#define SYSCALL_IO_BUFFER_SIZE 256ULL
#define SYSCALL_PATH_BUFFER_SIZE VFS_PATH_MAX
#define SYSCALL_MAX_RETURN_VALUE 0x7FFFFFFFFFFFFFFFULL
#define INPUT_EVENT_TYPE_SYNCHRONIZATION 0U
#define INPUT_EVENT_TYPE_KEY 1U
#define INPUT_EVENT_CODE_SYN_REPORT 0U

static const char g_syscall_version[] = NXU_KERNEL_NAME " " NXU_VERSION "\n";
#define SYSCALL_VERSION_LENGTH ((uint64_t)(sizeof(g_syscall_version) - 1U))

static syscall_result_t
syscall_return(uint64_t value)
{
	return (syscall_result_t) {
		.action = SYSCALL_ACTION_RETURN,
		.value = value
	};
}

static syscall_result_t
syscall_error(syscall_error_t error)
{
	return syscall_return(0ULL - (uint64_t)error);
}

static syscall_error_t
syscall_vfs_error(vfs_status_t status)
{
	switch (status) {
	case VFS_STATUS_NOT_FOUND: return SYSCALL_ERROR_NOT_FOUND;
	case VFS_STATUS_NO_MEMORY: return SYSCALL_ERROR_NO_MEMORY;
	case VFS_STATUS_NO_SPACE: return SYSCALL_ERROR_NO_SPACE;
	case VFS_STATUS_BAD_FD: return SYSCALL_ERROR_BAD_FD;
	case VFS_STATUS_EXISTS: return SYSCALL_ERROR_EXISTS;
	case VFS_STATUS_NOT_SUPPORTED: return SYSCALL_ERROR_NOT_SUPPORTED;
	case VFS_STATUS_INVALID: return SYSCALL_ERROR_INVALID_ARGUMENT;
	case VFS_STATUS_BUSY: return SYSCALL_ERROR_BUSY;
	default: return SYSCALL_ERROR_IO;
	}
}

static syscall_result_t
syscall_exit(uint64_t status)
{
	if (!proc_exit_current(status)) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	return (syscall_result_t) {
		.action = SYSCALL_ACTION_EXIT,
		.value = status
	};
}

static syscall_result_t
syscall_write(uint64_t file_descriptor, uint64_t user_buffer, uint64_t length)
{
	if (length > SYSCALL_MAX_RETURN_VALUE) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	if (length != 0ULL && length - 1ULL > UINT64_MAX - user_buffer) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	char buffer[SYSCALL_IO_BUFFER_SIZE];
	uint64_t written = 0ULL;

	while (written < length) {
		uint64_t chunk_size = length - written;
		if (chunk_size > sizeof(buffer)) chunk_size = sizeof(buffer);
		if (!vm_copy_from_user(buffer, user_buffer + written, chunk_size)) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

		if (file_descriptor == SYSCALL_STDOUT || file_descriptor == SYSCALL_STDERR) {
			for (uint64_t index = 0ULL; index < chunk_size; index++) kputc(buffer[index]);
			written += chunk_size;
			continue;
		}

		if (file_descriptor < VFS_FD_FIRST_FILE || file_descriptor >= VFS_FD_MAX) return syscall_error(SYSCALL_ERROR_BAD_FD);

		uint64_t file_written = 0ULL;
		vfs_status_t status = vfs_write(&current_proc()->p_fd, (uint32_t)file_descriptor, buffer, chunk_size, &file_written);
		if (status != VFS_STATUS_OK) return syscall_error(syscall_vfs_error(status));
		written += file_written;
		if (file_written != chunk_size) break;
	}

	return syscall_return(written);
}

static syscall_result_t
syscall_read(uint64_t file_descriptor, uint64_t user_buffer, uint64_t length)
{
	if (file_descriptor < VFS_FD_FIRST_FILE || file_descriptor >= VFS_FD_MAX) return syscall_error(SYSCALL_ERROR_BAD_FD);
	if (length > SYSCALL_MAX_RETURN_VALUE) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	if (length != 0ULL && length - 1ULL > UINT64_MAX - user_buffer) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	char buffer[SYSCALL_IO_BUFFER_SIZE];
	uint64_t complete = 0ULL;

	while (complete < length) {
		uint64_t chunk_size = length - complete;
		if (chunk_size > sizeof(buffer)) chunk_size = sizeof(buffer);

		uint64_t read_size = 0ULL;
		vfs_status_t status = vfs_read(&current_proc()->p_fd, (uint32_t)file_descriptor, buffer, chunk_size, &read_size);
		if (status != VFS_STATUS_OK) return syscall_error(syscall_vfs_error(status));
		if (read_size == 0ULL) break;
		if (!vm_copy_to_user(user_buffer + complete, buffer, read_size)) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
		complete += read_size;
		if (read_size < chunk_size) break;
	}

	return syscall_return(complete);
}

static syscall_result_t
syscall_open(uint64_t user_path, uint64_t flags)
{
	char path[SYSCALL_PATH_BUFFER_SIZE];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	uint32_t descriptor;
	vfs_status_t status = vfs_open(&current_proc()->p_fd, path, (uint32_t)flags, &descriptor);
	if (status != VFS_STATUS_OK) return syscall_error(syscall_vfs_error(status));
	return syscall_return(descriptor);
}

static syscall_result_t
syscall_close(uint64_t descriptor)
{
	if (descriptor < VFS_FD_FIRST_FILE || descriptor >= VFS_FD_MAX) return syscall_error(SYSCALL_ERROR_BAD_FD);
	vfs_status_t status = vfs_close(&current_proc()->p_fd, (uint32_t)descriptor);
	return status == VFS_STATUS_OK ? syscall_return(0ULL) : syscall_error(syscall_vfs_error(status));
}

static syscall_result_t
syscall_spawn(uint64_t user_path, uint64_t user_name)
{
	char path[VFS_PATH_MAX];
	char name[PROC_NAME_MAX];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	if (!vm_copy_string_from_user(name, user_name, sizeof(name))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	proc_t child;
	exec_status_t status = exec_spawn(current_proc(), path, name, &child);
	if (status == EXEC_STATUS_NOT_FOUND) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	if (status == EXEC_STATUS_NO_MEMORY) return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	if (status != EXEC_STATUS_OK || child == 0) return syscall_error(SYSCALL_ERROR_IO);
	return syscall_return(child->p_ident.pid);
}

static syscall_result_t
syscall_waitpid(uint64_t pid, uint64_t user_status)
{
	uint64_t status;
	if (!proc_reap(current_proc(), (proc_id_t)pid, &status)) return syscall_error(SYSCALL_ERROR_AGAIN);

	if (user_status != 0ULL && !vm_copy_to_user(user_status, &status, sizeof(status))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(pid);
}

static syscall_result_t
syscall_get_boot_args(uint64_t user_buffer, uint64_t capacity)
{
	const char *arguments = boot_args_raw();
	uint64_t length = strlen(arguments) + 1ULL;
	if (capacity < length) return syscall_error(SYSCALL_ERROR_NO_SPACE);
	if (!vm_copy_to_user(user_buffer, arguments, length)) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(length - 1ULL);
}

static syscall_result_t
syscall_klog_read(uint64_t user_cursor, uint64_t user_buffer, uint64_t capacity)
{
	uint64_t cursor;
	if (!vm_copy_from_user(&cursor, user_cursor, sizeof(cursor))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	if (capacity > SYSCALL_IO_BUFFER_SIZE) capacity = SYSCALL_IO_BUFFER_SIZE;

	char buffer[SYSCALL_IO_BUFFER_SIZE];
	uint64_t read_size;
	if (!kconsole_history_read(&cursor, buffer, capacity, &read_size)) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	if (read_size != 0ULL && !vm_copy_to_user(user_buffer, buffer, read_size)) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	if (!vm_copy_to_user(user_cursor, &cursor, sizeof(cursor))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(read_size);
}

/*
 * Routine:     syscall_readdir
 * Purpose:
 *              Export one VFS directory entry through the stable userspace
 *              ABI. Zero means end-of-directory; one means an entry was
 *              copied successfully.
 */
static syscall_result_t
syscall_readdir(uint64_t descriptor, uint64_t user_entry)
{
	if (descriptor < VFS_FD_FIRST_FILE || descriptor >= VFS_FD_MAX) return syscall_error(SYSCALL_ERROR_BAD_FD);

	vfs_dirent_t entry;
	vfs_status_t status = vfs_readdir(&current_proc()->p_fd, (uint32_t)descriptor, &entry);
	if (status == VFS_STATUS_END_OF_DIRECTORY) return syscall_return(0ULL);
	if (status != VFS_STATUS_OK) return syscall_error(syscall_vfs_error(status));

	nxu_dirent_t user = {
		.inode = entry.inode,
		.type = NXU_DIRENT_TYPE_UNKNOWN,
		.name_length = entry.name_length
	};

	if (entry.type == VNODE_TYPE_REGULAR) user.type = NXU_DIRENT_TYPE_REGULAR;
	if (entry.type == VNODE_TYPE_DIRECTORY) user.type = NXU_DIRENT_TYPE_DIRECTORY;

	for (uint32_t index = 0U; index <= entry.name_length; index++) user.name[index] = entry.name[index];
	if (!vm_copy_to_user(user_entry, &user, sizeof(user))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(1ULL);
}

static syscall_result_t
syscall_seek(uint64_t descriptor, uint64_t offset)
{
	if (descriptor < VFS_FD_FIRST_FILE || descriptor >= VFS_FD_MAX) return syscall_error(SYSCALL_ERROR_BAD_FD);
	vfs_status_t status = vfs_seek(&current_proc()->p_fd, (uint32_t)descriptor, offset);
	return status == VFS_STATUS_OK ? syscall_return(offset) : syscall_error(syscall_vfs_error(status));
}

static syscall_result_t
syscall_stat(uint64_t user_path, uint64_t user_stat)
{
	char path[VFS_PATH_MAX];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	vnode_t vnode;
	vfs_status_t status = vfs_lookup(path, &vnode);
	if (status != VFS_STATUS_OK) return syscall_error(syscall_vfs_error(status));

	nxu_stat_t stat = {
		.inode = vnode->v_id,
		.size = vnode->v_size,
		.type = NXU_STAT_TYPE_UNKNOWN,
		.reserved = 0U
	};

	if (vnode->v_type == VNODE_TYPE_REGULAR) stat.type = NXU_STAT_TYPE_REGULAR;
	if (vnode->v_type == VNODE_TYPE_DIRECTORY) stat.type = NXU_STAT_TYPE_DIRECTORY;
	if (vnode->v_type == VNODE_TYPE_CHARACTER) stat.type = NXU_STAT_TYPE_CHARACTER;
	if (vnode->v_type == VNODE_TYPE_BLOCK) stat.type = NXU_STAT_TYPE_BLOCK;
	vnode_rele(vnode);

	if (!vm_copy_to_user(user_stat, &stat, sizeof(stat))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_mkdir(uint64_t user_path)
{
	char path[VFS_PATH_MAX];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	vfs_status_t status = vfs_mkdir(path);
	return status == VFS_STATUS_OK ? syscall_return(0ULL) : syscall_error(syscall_vfs_error(status));
}

static bool
syscall_recovery_privileged(void)
{
	return boot_mode_is_triage_os() && proc_selfpid() == 1U;
}

static syscall_result_t
syscall_recovery_fs_mount_info(uint64_t index, uint64_t user_info)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (index > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	vfs_mount_info_t mount;
	if (!vfs_mount_get((uint32_t)index, &mount)) return syscall_error(SYSCALL_ERROR_NOT_FOUND);

	nxu_recovery_fs_mount_info_t info = {
		.device_index = mount.device_index,
		.read_only = mount.read_only ? 1U : 0U
	};
	memcpy(info.filesystem, mount.filesystem, sizeof(info.filesystem));
	memcpy(info.path, mount.path, sizeof(info.path));
	memcpy(info.device, mount.device, sizeof(info.device));

	if (!vm_copy_to_user(user_info, &info, sizeof(info))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_recovery_fs_mount(uint64_t user_filesystem, uint64_t device_index, uint64_t user_path)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (device_index > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	char filesystem[NXU_RECOVERY_FS_TYPE_MAX + 1U];
	char path[VFS_PATH_MAX];
	if (!vm_copy_string_from_user(filesystem, user_filesystem, sizeof(filesystem))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	block_device_t device = block_device_get((uint32_t)device_index);
	if (device == 0) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	vfs_status_t status = vfs_mount(filesystem, device, path);
	return status == VFS_STATUS_OK ? syscall_return(0ULL) : syscall_error(syscall_vfs_error(status));
}

static syscall_result_t
syscall_recovery_fs_unmount(uint64_t user_path)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);

	char path[VFS_PATH_MAX];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	if (path[0] == '/' && path[1] == '\0') return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	vfs_status_t status = vfs_unmount(path);
	return status == VFS_STATUS_OK ? syscall_return(0ULL) : syscall_error(syscall_vfs_error(status));
}

static syscall_result_t
syscall_recovery_block_info(uint64_t index, uint64_t user_info)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (index > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	block_device_t device = block_device_get((uint32_t)index);
	if (device == 0) return syscall_error(SYSCALL_ERROR_NOT_FOUND);

	nxu_recovery_block_info_t info = {
		.sector_count = device->sector_count,
		.sector_size = device->sector_size,
		.logical_block_size = device->logical_block_size,
		.read_only = device->read_only ? 1U : 0U,
		.reserved = 0U
	};
	memcpy(info.name, device->name, sizeof(info.name));
	if (!vm_copy_to_user(user_info, &info, sizeof(info))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_recovery_fs_space_info(uint64_t user_path, uint64_t user_info)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);

	char path[VFS_PATH_MAX];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	vfs_space_info_t space;
	vfs_status_t status = vfs_space_info(path, &space);
	if (status != VFS_STATUS_OK) return syscall_error(syscall_vfs_error(status));

	nxu_recovery_fs_space_info_t info = {
		.total_bytes = space.total_bytes,
		.free_bytes = space.free_bytes,
		.block_size = space.block_size,
		.read_only = space.read_only ? 1U : 0U
	};
	if (!vm_copy_to_user(user_info, &info, sizeof(info))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_recovery_block_layout_info(uint64_t index, uint64_t user_info)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (index > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	block_device_t device = block_device_get((uint32_t)index);
	if (device == 0) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	block_layout_info_t layout;
	if (!block_layout_inspect(device, &layout)) return syscall_error(SYSCALL_ERROR_IO);

	nxu_recovery_block_layout_info_t info = {
		.scheme = (uint32_t)layout.scheme,
		.partition_count = layout.partition_count
	};
	if (!vm_copy_to_user(user_info, &info, sizeof(info))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_recovery_block_partition_info(uint64_t device_index, uint64_t partition_index, uint64_t user_info)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (device_index > UINT32_MAX || partition_index > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	block_device_t device = block_device_get((uint32_t)device_index);
	if (device == 0) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	block_partition_info_t partition;
	if (!block_partition_get(device, (uint32_t)partition_index, &partition)) return syscall_error(SYSCALL_ERROR_NOT_FOUND);

	nxu_recovery_block_partition_info_t info = {
		.start_sector = partition.start_sector,
		.sector_count = partition.sector_count,
		.scheme = (uint32_t)partition.scheme,
		.index = partition.index,
		.type = partition.type,
		.reserved = 0U
	};
	memcpy(info.name, partition.name, sizeof(info.name));
	if (!vm_copy_to_user(user_info, &info, sizeof(info))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_recovery_block_health_info(uint64_t device_index, uint64_t user_info)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (device_index > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	block_device_t device = block_device_get((uint32_t)device_index);
	if (device == 0) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	uint64_t errors = device->read_errors + device->write_errors + device->flush_errors;
	nxu_recovery_block_health_info_t info = {
		.read_operations = device->read_operations,
		.write_operations = device->write_operations,
		.flush_operations = device->flush_operations,
		.read_errors = device->read_errors,
		.write_errors = device->write_errors,
		.flush_errors = device->flush_errors,
		.online = device->registered ? 1U : 0U,
		.healthy = device->registered && errors == 0ULL ? 1U : 0U,
		.read_only = device->read_only ? 1U : 0U,
		.flush_supported = device->flush_supported ? 1U : 0U
	};
	if (!vm_copy_to_user(user_info, &info, sizeof(info))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_recovery_block_verify(uint64_t device_index)
{
	if (!syscall_recovery_privileged()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (device_index > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	block_device_t device = block_device_get((uint32_t)device_index);
	if (device == 0) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	return block_device_verify(device) ? syscall_return(0ULL) : syscall_error(SYSCALL_ERROR_IO);
}

static syscall_result_t
syscall_recovery_display_info(uint64_t user_info)
{
	if (!boot_mode_is_triage_os()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	display_device_t *display = display_primary();
	if (display == 0 || display->framebuffer == 0) return syscall_error(SYSCALL_ERROR_IO);

	nxu_recovery_display_info_t info = {
		.width = display->width,
		.height = display->height,
		.stride = display->width,
		.pixel_format = 1U
	};

	if (!vm_copy_to_user(user_info, &info, sizeof(info))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_recovery_present(uint64_t user_pixels, uint64_t stride, uint64_t x, uint64_t y, uint64_t width, uint64_t height)
{
	if (!boot_mode_is_triage_os()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	display_device_t *display = display_primary();
	if (display == 0 || display->framebuffer == 0) return syscall_error(SYSCALL_ERROR_IO);
	if (stride < display->width || x >= display->width || y >= display->height || width == 0ULL || height == 0ULL) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	if (width > display->width - x || height > display->height - y) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	for (uint64_t row = 0ULL; row < height; row++) {
		uint64_t pixel_offset = (y + row) * stride + x;
		if (pixel_offset > (UINT64_MAX / sizeof(uint32_t))) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
		uint64_t byte_offset = pixel_offset * sizeof(uint32_t);
		if (user_pixels > UINT64_MAX - byte_offset) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
		uint32_t *destination = display->framebuffer + (y + row) * display->stride + x;
		if (!vm_copy_from_user(destination, user_pixels + byte_offset, width * sizeof(uint32_t))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	}

	return display_present(display, (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height) ? syscall_return(0ULL) : syscall_error(SYSCALL_ERROR_IO);
}

static syscall_result_t
syscall_recovery_input(uint64_t user_event)
{
	if (!boot_mode_is_triage_os()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);

	/*
	 * Drain already-normalized input before polling the VirtIO transports. A
	 * pointer packet can contain several raw events, and polling every device
	 * again for each userspace read adds needless latency to recovery UI.
	 */
	input_event_t input;
	if (!input_read(&input)) {
		virtio_input_service();
		if (!input_read(&input)) return syscall_return(0ULL);
	}

	for (;;) {
		nxu_recovery_input_event_t event;

		if (
			input.device_class == INPUT_DEVICE_KEYBOARD &&
			input.type == INPUT_EVENT_TYPE_KEY
		) {
			event = (nxu_recovery_input_event_t) {
				.kind = NXU_RECOVERY_INPUT_KEY,
				.code = input.code,
				.value = input.value,
				.x = 0,
				.y = 0,
				.buttons = mouse_buttons(),
				.modifiers = keyboard_modifiers()
			};
		} else if (
			input.device_class == INPUT_DEVICE_MOUSE &&
			input.type == INPUT_EVENT_TYPE_SYNCHRONIZATION &&
			input.code == INPUT_EVENT_CODE_SYN_REPORT
		) {
			event = (nxu_recovery_input_event_t) {
				.kind = NXU_RECOVERY_INPUT_POINTER,
				.code = 0U,
				.value = 0,
				.x = (int32_t)mouse_x(),
				.y = (int32_t)mouse_y(),
				.buttons = mouse_buttons(),
				.modifiers = keyboard_modifiers()
			};
		} else {
			if (!input_read(&input)) return syscall_return(0ULL);
			continue;
		}

		if (!vm_copy_to_user(user_event, &event, sizeof(event))) {
			return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
		}

		return syscall_return(1ULL);
	}

	return syscall_return(0ULL);
}

static syscall_result_t
syscall_system_reset(void)
{
	if (!boot_mode_is_triage_os()) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	register uint64_t x0 __asm__("x0") = 0x84000009ULL;
	__asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
	return syscall_error(SYSCALL_ERROR_IO);
}

static syscall_result_t
syscall_unlink(uint64_t user_path)
{
	char path[VFS_PATH_MAX];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	vfs_status_t status = vfs_unlink(path);
	return status == VFS_STATUS_OK ? syscall_return(0ULL) : syscall_error(syscall_vfs_error(status));
}

static syscall_result_t
syscall_get_version(uint64_t user_buffer, uint64_t capacity)
{
	if (capacity < SYSCALL_VERSION_LENGTH) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	if (!vm_copy_to_user(user_buffer, g_syscall_version, SYSCALL_VERSION_LENGTH)) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	return syscall_return(SYSCALL_VERSION_LENGTH);
}

syscall_result_t
syscall_dispatch(const syscall_request_t *request)
{
	if (request == 0) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	switch (request->number) {
	case SYSCALL_EXIT: return syscall_exit(request->arguments[0]);
	case SYSCALL_WRITE: return syscall_write(request->arguments[0], request->arguments[1], request->arguments[2]);
	case SYSCALL_GET_VERSION: return syscall_get_version(request->arguments[0], request->arguments[1]);
	case SYSCALL_OPEN: return syscall_open(request->arguments[0], request->arguments[1]);
	case SYSCALL_READ: return syscall_read(request->arguments[0], request->arguments[1], request->arguments[2]);
	case SYSCALL_CLOSE: return syscall_close(request->arguments[0]);
	case SYSCALL_SPAWN: return syscall_spawn(request->arguments[0], request->arguments[1]);
	case SYSCALL_WAITPID: return syscall_waitpid(request->arguments[0], request->arguments[1]);
	case SYSCALL_GETPID: return syscall_return(proc_selfpid());
	case SYSCALL_YIELD: return sched_yield() ? syscall_return(0ULL) : syscall_error(SYSCALL_ERROR_IO);
	case SYSCALL_GET_BOOT_ARGS: return syscall_get_boot_args(request->arguments[0], request->arguments[1]);
	case SYSCALL_KLOG_READ: return syscall_klog_read(request->arguments[0], request->arguments[1], request->arguments[2]);
	case SYSCALL_UNLINK: return syscall_unlink(request->arguments[0]);
	case SYSCALL_SYNC: return vfs_sync_all() == VFS_STATUS_OK ? syscall_return(0ULL) : syscall_error(SYSCALL_ERROR_IO);
	case SYSCALL_UPTIME_US: return syscall_return(timer_ticks_to_microseconds(timer_get_ticks()));
	case SYSCALL_READDIR: return syscall_readdir(request->arguments[0], request->arguments[1]);
	case SYSCALL_RECOVERY_DISPLAY_INFO: return syscall_recovery_display_info(request->arguments[0]);
	case SYSCALL_RECOVERY_PRESENT: return syscall_recovery_present(request->arguments[0], request->arguments[1], request->arguments[2], request->arguments[3], request->arguments[4], request->arguments[5]);
	case SYSCALL_RECOVERY_INPUT: return syscall_recovery_input(request->arguments[0]);
	case SYSCALL_SYSTEM_RESET: return syscall_system_reset();
	case SYSCALL_SEEK: return syscall_seek(request->arguments[0], request->arguments[1]);
	case SYSCALL_STAT: return syscall_stat(request->arguments[0], request->arguments[1]);
	case SYSCALL_MKDIR: return syscall_mkdir(request->arguments[0]);
	case SYSCALL_RECOVERY_FS_MOUNT_INFO: return syscall_recovery_fs_mount_info(request->arguments[0], request->arguments[1]);
	case SYSCALL_RECOVERY_FS_MOUNT: return syscall_recovery_fs_mount(request->arguments[0], request->arguments[1], request->arguments[2]);
	case SYSCALL_RECOVERY_FS_UNMOUNT: return syscall_recovery_fs_unmount(request->arguments[0]);
	case SYSCALL_RECOVERY_BLOCK_INFO: return syscall_recovery_block_info(request->arguments[0], request->arguments[1]);
	case SYSCALL_RECOVERY_FS_SPACE_INFO: return syscall_recovery_fs_space_info(request->arguments[0], request->arguments[1]);
	case SYSCALL_RECOVERY_BLOCK_LAYOUT_INFO: return syscall_recovery_block_layout_info(request->arguments[0], request->arguments[1]);
	case SYSCALL_RECOVERY_BLOCK_PARTITION_INFO: return syscall_recovery_block_partition_info(request->arguments[0], request->arguments[1], request->arguments[2]);
	case SYSCALL_RECOVERY_BLOCK_HEALTH_INFO: return syscall_recovery_block_health_info(request->arguments[0], request->arguments[1]);
	case SYSCALL_RECOVERY_BLOCK_VERIFY: return syscall_recovery_block_verify(request->arguments[0]);
	default: return syscall_error(SYSCALL_ERROR_UNKNOWN);
	}
}
