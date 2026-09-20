#include <kern/syscall/syscall.h>

#include <mach/machine/system.h>
#include <mach/machine/timer.h>
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
#include <kern/console/display_owner.h>
#include <kern/loader/elf.h>
#include <kern/ipc/ipc_init.h>
#include <kern/ipc/ipc_kmsg.h>
#include <kern/ipc/ipc_port.h>
#include <kern/ipc/ipc_space.h>
#include <kern/ipc/ipc_types.h>
#include <kern/ipc/shm_registry.h>
#include <kern/ipc/socket.h>
#include <kern/logging/version.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/waitq.h>
#include <mach/machine/user.h>
#include <kern/sched_prism/sched.h>
#include <vfs/vfs.h>
#include <vm/user_copy.h>
#include <vm/vm_map.h>
#include <vm/vm_shm.h>

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

static syscall_error_t
syscall_ipc_error(ipc_return_t status)
{
	switch (status) {
	case IPC_INVALID_ARGUMENT: return SYSCALL_ERROR_INVALID_ARGUMENT;
	case IPC_NO_MEMORY: return SYSCALL_ERROR_NO_MEMORY;
	case IPC_MESSAGE_TOO_LARGE: return SYSCALL_ERROR_INVALID_ARGUMENT;
	case IPC_PORT_INACTIVE: return SYSCALL_ERROR_NOT_FOUND;
	case IPC_QUEUE_EMPTY: return SYSCALL_ERROR_AGAIN;
	case IPC_QUEUE_FULL: return SYSCALL_ERROR_BUSY;
	case IPC_BUFFER_TOO_SMALL: return SYSCALL_ERROR_INVALID_ARGUMENT;
	case IPC_OVERFLOW: return SYSCALL_ERROR_INVALID_ARGUMENT;
	case IPC_SPACE_FULL: return SYSCALL_ERROR_NO_SPACE;
	case IPC_NAME_INVALID: return SYSCALL_ERROR_NOT_FOUND;
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
syscall_getppid(void)
{
	return syscall_return((uint64_t)proc_ppid(current_proc()));
}

/*
 * fork: a copy-on-write copy of the calling process. The child resumes right
 * after this system call with 0 in the result register; the parent gets the
 * child's PID.
 */
static syscall_result_t
syscall_fork(void)
{
#if MACHINE_USER_CONTEXT
	proc_t child = 0;

	if (!proc_fork(current_proc(), &child)) return syscall_error(SYSCALL_ERROR_NO_MEMORY);

	return syscall_return((uint64_t)child->p_ident.pid);
#else
	return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
#endif
}

#if MACHINE_USER_CONTEXT

/*
 * syscall_copy_exec_args
 *
 * Read argv (a NULL-terminated array of string pointers) out of user memory
 * into args, packed as loader_exec_args_t wants it. A missing argv becomes
 * {path}, so a program always sees argc >= 1.
 */
static syscall_error_t
syscall_copy_exec_args(loader_exec_args_t *args, const char *path, uint64_t user_argv)
{
	args->argc = 0U;
	args->length = 0U;

	if (user_argv == 0ULL) {
		uint32_t length = (uint32_t)strlen(path) + 1U;
		if (length > sizeof(args->strings)) return SYSCALL_ERROR_INVALID_ARGUMENT;

		memcpy(args->strings, path, length);
		args->argc = 1U;
		args->length = length;
		return (syscall_error_t)0;
	}

	for (uint32_t index = 0U; ; index++) {
		if (index >= NXU_EXEC_ARGV_MAX) return SYSCALL_ERROR_INVALID_ARGUMENT;

		uint64_t user_string;
		if (!vm_copy_from_user(&user_string, user_argv + (uint64_t)index * sizeof(uint64_t), sizeof(user_string))) return SYSCALL_ERROR_BAD_ADDRESS;
		if (user_string == 0ULL) break;

		uint64_t remaining = sizeof(args->strings) - args->length;
		if (remaining == 0ULL) return SYSCALL_ERROR_INVALID_ARGUMENT;

		if (!vm_copy_string_from_user(args->strings + args->length, user_string, remaining)) return SYSCALL_ERROR_BAD_ADDRESS;

		args->length += (uint32_t)strlen(args->strings + args->length) + 1U;
		args->argc++;
	}

	if (args->argc == 0U) return SYSCALL_ERROR_INVALID_ARGUMENT;

	return (syscall_error_t)0;
}

#endif

/*
 * exec: replace the calling program with the executable at path, passing
 * argv (NULL for just {path}). Does not return on success -- the caller's
 * registers now describe the new program's entry.
 */
static syscall_result_t
syscall_exec(uint64_t user_path, uint64_t user_argv)
{
#if MACHINE_USER_CONTEXT
	char path[VFS_PATH_MAX];
	if (!vm_copy_string_from_user(path, user_path, sizeof(path))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	loader_exec_args_t *args = kmalloc(sizeof(*args));
	if (args == 0) return syscall_error(SYSCALL_ERROR_NO_MEMORY);

	syscall_error_t error = syscall_copy_exec_args(args, path, user_argv);
	if (error != (syscall_error_t)0) {
		(void)kfree(args);
		return syscall_error(error);
	}

	loader_status_t status = loader_exec(current_proc(), path, args);
	(void)kfree(args);

	switch (status) {
	case LOADER_STATUS_OK:
		return (syscall_result_t) { .action = SYSCALL_ACTION_KEEP_FRAME, .value = 0ULL };
	case LOADER_STATUS_NOT_FOUND: return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	case LOADER_STATUS_NO_MEMORY: return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	case LOADER_STATUS_BUSY: return syscall_error(SYSCALL_ERROR_BUSY);
	case LOADER_STATUS_INVALID: return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	default: return syscall_error(SYSCALL_ERROR_IO);
	}
#else
	(void)user_path;
	(void)user_argv;
	return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
#endif
}

static syscall_result_t
syscall_kill(uint64_t pid, uint64_t signal)
{
#if MACHINE_USER_CONTEXT
	if (pid > PROC_PID_MAX || signal >= NXU_NSIG) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	switch (signal_send(current_proc(), (proc_id_t)pid, (uint32_t)signal)) {
	case SIGNAL_SEND_OK: return syscall_return(0ULL);
	case SIGNAL_SEND_NO_SUCH_PROCESS: return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	case SIGNAL_SEND_DENIED: return syscall_error(SYSCALL_ERROR_DENIED);
	case SIGNAL_SEND_NOT_SUPPORTED: return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	default: return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}
#else
	(void)pid;
	(void)signal;
	return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
#endif
}

/*
 * sigaction: read (old_action) and/or replace (action) the disposition of a
 * signal. Either pointer may be NULL.
 */
static syscall_result_t
syscall_sigaction(uint64_t signal, uint64_t user_action, uint64_t user_old_action)
{
#if MACHINE_USER_CONTEXT
	if (!signal_valid((uint32_t)signal) || signal >= NXU_NSIG) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	proc_t proc = current_proc();
	nxu_sigaction_t previous;
	nxu_sigaction_t replacement;

	if (!signal_get_action(proc, (uint32_t)signal, &previous)) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	if (user_action != 0ULL) {
		if (!vm_copy_from_user(&replacement, user_action, sizeof(replacement))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
		if (!signal_set_action(proc, (uint32_t)signal, &replacement)) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}

	if (user_old_action != 0ULL && !vm_copy_to_user(user_old_action, &previous, sizeof(previous))) {
		return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	}

	return syscall_return(0ULL);
#else
	(void)signal;
	(void)user_action;
	(void)user_old_action;
	return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
#endif
}

/*
 * sigprocmask: change the calling thread's blocked-signal mask and return the
 * previous one. SIGKILL cannot be blocked.
 */
static syscall_result_t
syscall_sigprocmask(uint64_t how, uint64_t set)
{
#if MACHINE_USER_CONTEXT
	thread_t thread = current_thread();
	if (thread == 0) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	uint32_t previous = thread->sig_blocked;
	uint32_t change = signal_sanitize_mask((uint32_t)set);

	switch (how) {
	case NXU_SIG_BLOCK: thread->sig_blocked = previous | change; break;
	case NXU_SIG_UNBLOCK: thread->sig_blocked = previous & ~change; break;
	case NXU_SIG_SETMASK: thread->sig_blocked = change; break;
	default: return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}

	return syscall_return((uint64_t)previous);
#else
	(void)how;
	(void)set;
	return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
#endif
}

/*
 * sigreturn: the tail of every signal handler. Restores the context the
 * handler interrupted. A frame that does not check out is a program bug (or
 * an attack), and ends the process.
 */
static syscall_result_t
syscall_sigreturn(void)
{
#if MACHINE_USER_CONTEXT
	uint32_t saved_mask;

	if (!machine_user_signal_pop(&saved_mask)) {
		if (proc_exit_current(NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV))) {
			return (syscall_result_t) { .action = SYSCALL_ACTION_EXIT, .value = NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV) };
		}

		return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}

	current_thread()->sig_blocked = signal_sanitize_mask(saved_mask);

	return (syscall_result_t) { .action = SYSCALL_ACTION_KEEP_FRAME, .value = 0ULL };
#else
	return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
#endif
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
	loader_status_t status = loader_spawn(current_proc(), path, name, &child);
	if (status == LOADER_STATUS_NOT_FOUND) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	if (status == LOADER_STATUS_NO_MEMORY) return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	if (status != LOADER_STATUS_OK || child == 0) return syscall_error(SYSCALL_ERROR_IO);
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
	if (!boot_mode_is_triage_os() && !display_owner_is(proc_selfpid())) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
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
	if (!boot_mode_is_triage_os() && !display_owner_is(proc_selfpid())) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
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
	if (!boot_mode_is_triage_os() && !display_owner_is(proc_selfpid())) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);

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
	machine_system_reset();
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

static syscall_result_t
syscall_ipc_port_allocate(void)
{
	ipc_port_t port;
	ipc_return_t status = ipc_port_alloc(&port);
	if (status != IPC_SUCCESS) return syscall_error(syscall_ipc_error(status));

	uint32_t name;
	status = ipc_space_insert_port(&current_proc()->p_ipc, port, &name);
	if (status != IPC_SUCCESS) {
		ipc_port_release(port);
		return syscall_error(syscall_ipc_error(status));
	}

	return syscall_return(name);
}

static syscall_result_t
syscall_ipc_port_deallocate(uint64_t name)
{
	if (name == 0ULL || name > IPC_SPACE_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	ipc_port_t port;
	ipc_return_t status = ipc_space_remove(&current_proc()->p_ipc, (uint32_t)name, &port);
	if (status != IPC_SUCCESS) return syscall_error(syscall_ipc_error(status));

	ipc_port_release(port);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_ipc_bootstrap_port(void)
{
	proc_t proc = current_proc();
	if (proc->p_ipc_bootstrap_name == IPC_SPACE_NAME_INVALID) return syscall_error(SYSCALL_ERROR_NOT_FOUND);
	return syscall_return(proc->p_ipc_bootstrap_name);
}

/*
 * xfer_name, when not IPC_SPACE_NAME_INVALID, must be a name the caller
 * currently holds; whatever it refers to is transferred to the receiver on
 * a successful ipc_receive, exactly like Mach right passing but limited to
 * one transferable name per message.
 */
static syscall_result_t
syscall_ipc_send(uint64_t dest_name, uint64_t user_buffer, uint64_t length, uint64_t xfer_name)
{
	if (dest_name == 0ULL || dest_name > IPC_SPACE_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	if (length == 0ULL || length > IPC_KMSG_MAX_INLINE_SIZE) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	if (xfer_name > IPC_SPACE_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	/* Heap, not stack: syscall handlers run on a 16 KiB kernel stack (see
	 * THREAD_KERNEL_STACK_SIZE) and IPC_KMSG_MAX_INLINE_SIZE is 4 KiB. */
	void *buffer = kmalloc(length);
	if (buffer == 0) return syscall_error(SYSCALL_ERROR_NO_MEMORY);

	if (!vm_copy_from_user(buffer, user_buffer, length)) {
		(void)kfree(buffer);
		return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	}

	proc_t proc = current_proc();

	ipc_port_t xfer_port = IPC_PORT_NULL;
	if (xfer_name != IPC_SPACE_NAME_INVALID) {
		ipc_return_t lookup_status = ipc_space_lookup_port(&proc->p_ipc, (uint32_t)xfer_name, &xfer_port);
		if (lookup_status != IPC_SUCCESS) {
			(void)kfree(buffer);
			return syscall_error(syscall_ipc_error(lookup_status));
		}
	}

	ipc_port_t dest_port;
	ipc_return_t status = ipc_space_lookup_port(&proc->p_ipc, (uint32_t)dest_name, &dest_port);
	if (status != IPC_SUCCESS) {
		if (xfer_port != IPC_PORT_NULL) ipc_port_release(xfer_port);
		(void)kfree(buffer);
		return syscall_error(syscall_ipc_error(status));
	}

	ipc_kmsg_t kmsg;
	status = ipc_kmsg_alloc(buffer, length, xfer_port, &kmsg);
	(void)kfree(buffer);
	if (status != IPC_SUCCESS) {
		if (xfer_port != IPC_PORT_NULL) ipc_port_release(xfer_port);
		ipc_port_release(dest_port);
		return syscall_error(syscall_ipc_error(status));
	}
	/* xfer_port's reference (if any) now belongs to kmsg. */

	status = ipc_port_enqueue(dest_port, kmsg);
	ipc_port_release(dest_port);
	if (status != IPC_SUCCESS) {
		ipc_kmsg_free(kmsg);
		return syscall_error(syscall_ipc_error(status));
	}

	return syscall_return(0ULL);
}

/*
 * Returns the received message's size on success (matching syscall_read's
 * convention), or SYSCALL_ERROR_AGAIN if port_name's queue is empty --
 * every event loop using this is expected to poll it in its own
 * nxu_yield() loop, the same pattern bootd already uses for nxu_waitpid.
 * user_out_xfer_name/user_out_xfer_type may be 0 to ignore a transfer.
 */
/*
 * Receive one message. With wait set the caller sleeps until one arrives
 * (interruptible by a signal, which returns SYSCALL_ERROR_INTERRUPTED);
 * without it an empty queue returns SYSCALL_ERROR_AGAIN immediately, the
 * poll-and-yield behaviour every existing service is written against.
 */
static syscall_result_t
syscall_ipc_receive_impl(uint64_t port_name, uint64_t user_buffer, uint64_t capacity, uint64_t user_out_xfer_name, uint64_t user_out_xfer_type, bool wait)
{
	if (port_name == 0ULL || port_name > IPC_SPACE_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	proc_t proc = current_proc();

	ipc_port_t port;
	ipc_return_t status = ipc_space_lookup_port(&proc->p_ipc, (uint32_t)port_name, &port);
	if (status != IPC_SUCCESS) return syscall_error(syscall_ipc_error(status));

	ipc_kmsg_t kmsg;
	bool interrupted = false;

	for (;;) {
		status = ipc_port_dequeue(port, &kmsg);
		if (status != IPC_QUEUE_EMPTY || !wait) break;

		/* Never sleep through a signal that is already waiting. */
		if (signal_pending_for(proc, current_thread()) || !ipc_port_wait(port)) {
			interrupted = true;
			break;
		}
	}

	ipc_port_release(port);
	if (interrupted) return syscall_error(SYSCALL_ERROR_INTERRUPTED);
	if (status != IPC_SUCCESS) return syscall_error(syscall_ipc_error(status));

	if ((uint64_t)kmsg->ikm_size > capacity) {
		/* The message is already off the queue and there is no way to put
		 * it back; a too-small buffer is a caller bug (NXPC protocols are
		 * fixed-size messages, so a conforming client never hits this) and
		 * the message is lost rather than silently truncated. */
		ipc_kmsg_free(kmsg);
		return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}

	if (kmsg->ikm_size != 0ULL && !vm_copy_to_user(user_buffer, kmsg->ikm_data, kmsg->ikm_size)) {
		ipc_kmsg_free(kmsg);
		return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	}

	uint32_t xfer_name = IPC_SPACE_NAME_INVALID;
	uint32_t xfer_type = 0U;

	if (kmsg->ikm_xfer_type == IPC_KMSG_XFER_PORT) {
		ipc_return_t insert_status = ipc_space_insert_port(&proc->p_ipc, kmsg->ikm_xfer_port, &xfer_name);
		if (insert_status == IPC_SUCCESS) {
			/* Ownership moved into proc->p_ipc; ipc_kmsg_free below must not
			 * also release it. */
			kmsg->ikm_xfer_type = IPC_KMSG_XFER_NONE;
			kmsg->ikm_xfer_port = IPC_PORT_NULL;
			xfer_type = (uint32_t)IPC_KMSG_XFER_PORT;
		}
		/* A full receiver space silently drops the transfer (freed below
		 * with the rest of the message) rather than failing the whole
		 * receive -- the payload the caller asked for is still valid. */
	}

	uint64_t received_size = (uint64_t)kmsg->ikm_size;
	ipc_kmsg_free(kmsg);

	if (user_out_xfer_name != 0ULL && !vm_copy_to_user(user_out_xfer_name, &xfer_name, sizeof(xfer_name))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	if (user_out_xfer_type != 0ULL && !vm_copy_to_user(user_out_xfer_type, &xfer_type, sizeof(xfer_type))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	return syscall_return(received_size);
}

static syscall_result_t
syscall_ipc_receive(uint64_t port_name, uint64_t user_buffer, uint64_t capacity, uint64_t user_out_xfer_name, uint64_t user_out_xfer_type)
{
	return syscall_ipc_receive_impl(port_name, user_buffer, capacity, user_out_xfer_name, user_out_xfer_type, false);
}

static syscall_result_t
syscall_ipc_receive_wait(uint64_t port_name, uint64_t user_buffer, uint64_t capacity, uint64_t user_out_xfer_name, uint64_t user_out_xfer_type)
{
	return syscall_ipc_receive_impl(port_name, user_buffer, capacity, user_out_xfer_name, user_out_xfer_type, true);
}

/*
 * wait: sleep until a child exits and reap it. pid names one child, or is
 * NXU_WAIT_ANY for whichever exits first. Returns the child's pid and stores
 * its exit status; fails with NOT_FOUND when there is nothing to wait for and
 * with INTERRUPTED when a signal arrives first. waitpid remains the polling
 * form (AGAIN while the child runs).
 */
static syscall_result_t
syscall_wait(uint64_t pid, uint64_t user_status)
{
	proc_t self = current_proc();
	thread_t thread = current_thread();
	proc_id_t target = pid == NXU_WAIT_ANY ? PROC_PID_INVALID : (proc_id_t)pid;

	if (pid != NXU_WAIT_ANY && pid > PROC_PID_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	for (;;) {
		proc_id_t reaped = 0U;
		uint64_t status = 0ULL;

		switch (proc_wait_child(self, target, &reaped, &status)) {
		case PROC_WAIT_REAPED:
			if (user_status != 0ULL && !vm_copy_to_user(user_status, &status, sizeof(status))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
			return syscall_return((uint64_t)reaped);

		case PROC_WAIT_NO_CHILD:
			return syscall_error(SYSCALL_ERROR_NOT_FOUND);

		case PROC_WAIT_NOT_YET:
			break;
		}

		/* Never sleep through a signal that is already waiting. */
		if (signal_pending_for(self, thread) || !waitq_block(&self->p_waitq, true)) {
			return syscall_error(SYSCALL_ERROR_INTERRUPTED);
		}
	}
}

static syscall_result_t
syscall_ipc_register_bootstrap(uint64_t port_name)
{
	if (proc_selfpid() != 1U) return syscall_error(SYSCALL_ERROR_NOT_SUPPORTED);
	if (port_name == 0ULL || port_name > IPC_SPACE_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	ipc_port_t port;
	ipc_return_t status = ipc_space_lookup_port(&current_proc()->p_ipc, (uint32_t)port_name, &port);
	if (status != IPC_SUCCESS) return syscall_error(syscall_ipc_error(status));

	ipc_set_bootstrap_registry_port(port);
	return syscall_return(0ULL);
}

static syscall_result_t
syscall_display_claim(void)
{
	return display_owner_claim(proc_selfpid()) ? syscall_return(0ULL) : syscall_error(SYSCALL_ERROR_BUSY);
}

static syscall_result_t
syscall_shm_create(uint64_t size)
{
	if (size == 0ULL || size > VM_SHM_MAX_BYTES) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	vm_shm_region_t region;
	if (!vm_shm_create(size, &region)) return syscall_error(SYSCALL_ERROR_NO_MEMORY);

	uint32_t id;
	if (!shm_registry_publish(region, &id)) {
		vm_shm_release(region);
		return syscall_error(SYSCALL_ERROR_NO_SPACE);
	}

	return syscall_return(id);
}

/*
 * Maps the region registered under id into the caller's own address space.
 * The handle reference shm_registry_attach hands back is intentionally
 * never released afterward -- there is no unmap syscall yet (out of scope
 * for this milestone) and no vm_address_space_destroy either, matching the
 * existing "no address-space teardown on process exit" gap already
 * documented in kern/tests/vm_shm_test.c.
 */
static syscall_result_t
syscall_shm_map(uint64_t id)
{
	if (id == 0ULL || id > UINT32_MAX) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	vm_shm_region_t region;
	if (!shm_registry_attach((uint32_t)id, &region)) return syscall_error(SYSCALL_ERROR_NOT_FOUND);

	proc_t proc = current_proc();
	uint64_t va;
	if (!vm_shm_map_into(proc_vm_map(proc), &proc->p_task.shm_next_va, region, VM_USER_PROTECTION_READ_WRITE, &va)) {
		vm_shm_release(region);
		return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	}

	return syscall_return(va);
}

static bool
syscall_mmap_protection(uint64_t prot_flags, vm_user_protection_t *protection)
{
	switch (prot_flags) {
	case NXU_MMAP_PROT_READ_WRITE: *protection = VM_USER_PROTECTION_READ_WRITE; return true;
	case NXU_MMAP_PROT_READ_ONLY: *protection = VM_USER_PROTECTION_READ_ONLY; return true;
	case NXU_MMAP_PROT_READ_EXECUTE: *protection = VM_USER_PROTECTION_READ_EXECUTE; return true;
	default: return false;
	}
}

static syscall_result_t
syscall_mmap(uint64_t size, uint64_t prot_flags)
{
	vm_user_protection_t protection;
	if (!syscall_mmap_protection(prot_flags, &protection)) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	if (size == 0ULL || size > VM_MAP_MAX_BYTES) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	uint64_t va;
	if (!vm_map_anon(proc_vm_map(current_proc()), size, protection, &va)) {
		return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	}

	return syscall_return(va);
}

static syscall_result_t
syscall_munmap(uint64_t address, uint64_t size)
{
	if (size == 0ULL) return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);

	if (!vm_map_free(proc_vm_map(current_proc()), address, size)) {
		return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}

	return syscall_return(0ULL);
}

/*
 * entry/arg follow pthread_create's shape: the new thread starts at entry
 * with arg in its initial x0. stack is the initial SP -- one past the last
 * valid byte, like kern/loader/elf.c's LOADER_USER_STACK_TOP -- and must fall
 * inside a region the caller itself nxu_mmap'd, the only general-purpose
 * way a user program can obtain a fresh stack today.
 */
static syscall_result_t
syscall_thread_create(uint64_t entry, uint64_t stack, uint64_t arg)
{
	if (entry == 0ULL || stack == 0ULL || (stack & 0xFULL) != 0ULL) {
		return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}

	proc_t proc = current_proc();

	if (!vm_map_lookup(proc_vm_map(proc), stack - 1ULL, 0, 0)) {
		return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);
	}

	thread_t thread;
	if (!thread_create(proc_task(proc), entry, stack, arg, &thread)) {
		return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	}

	if (!thread_stack_alloc(thread) || !sched_thread_start(thread)) {
		thread_deallocate(thread);
		return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	}

	uint64_t tid = (uint64_t)thread_tid(thread);
	thread_deallocate(thread);

	return syscall_return(tid);
}

/*
 * If the calling thread is the last one active in its task, this is
 * equivalent to syscall_exit -- routed through the very same path rather
 * than reimplementing fd-table/ipc-space teardown here. Otherwise only this
 * one thread is torn down; sched_exit_current() never returns.
 */
static syscall_result_t
syscall_thread_exit(uint64_t status)
{
	task_t task = proc_task(current_proc());

	if (task_active_thread_count(task) <= 1U) {
		return syscall_exit(status);
	}

	if (!sched_thread_terminate(current_thread())) {
		return syscall_error(SYSCALL_ERROR_IO);
	}

	sched_exit_current();
}

static syscall_result_t
syscall_thread_self(void)
{
	return syscall_return((uint64_t)thread_tid(current_thread()));
}

#define SYSCALL_SOCKET_NAME_BUFFER_SIZE (SOCKET_NAME_MAX + 1U)

/*
 * Installs socket in a fresh file object/descriptor, releasing socket on
 * any failure so the caller never has to -- mirrors how syscall_open's
 * vfs_open already folds file_alloc + filedesc_install into one step.
 */
static syscall_result_t
syscall_socket_install(socket_t socket)
{
	file_t file;
	if (file_alloc_socket(socket, &file) != VFS_STATUS_OK) {
		socket_close(socket);
		return syscall_error(SYSCALL_ERROR_NO_MEMORY);
	}

	uint32_t descriptor;
	if (filedesc_install(&current_proc()->p_fd, file, &descriptor) != VFS_STATUS_OK) {
		file_rele(file);
		return syscall_error(SYSCALL_ERROR_NO_SPACE);
	}

	return syscall_return(descriptor);
}

static syscall_result_t
syscall_socket_listen(uint64_t user_name, uint64_t backlog)
{
	char name[SYSCALL_SOCKET_NAME_BUFFER_SIZE];
	if (!vm_copy_string_from_user(name, user_name, sizeof(name))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	socket_t socket;
	if (!socket_create(&socket)) return syscall_error(SYSCALL_ERROR_NO_MEMORY);

	if (!socket_listen(socket, name, (uint32_t)backlog)) {
		socket_close(socket);
		return syscall_error(SYSCALL_ERROR_EXISTS);
	}

	return syscall_socket_install(socket);
}

static syscall_result_t
syscall_socket_connect(uint64_t user_name)
{
	char name[SYSCALL_SOCKET_NAME_BUFFER_SIZE];
	if (!vm_copy_string_from_user(name, user_name, sizeof(name))) return syscall_error(SYSCALL_ERROR_BAD_ADDRESS);

	socket_t socket;
	if (!socket_connect(name, true, &socket)) return syscall_error(SYSCALL_ERROR_NOT_FOUND);

	return syscall_socket_install(socket);
}

static syscall_result_t
syscall_socket_accept(uint64_t listen_descriptor)
{
	if (listen_descriptor < VFS_FD_FIRST_FILE || listen_descriptor >= VFS_FD_MAX) return syscall_error(SYSCALL_ERROR_BAD_FD);

	file_t listener;
	if (filedesc_get(&current_proc()->p_fd, (uint32_t)listen_descriptor, &listener) != VFS_STATUS_OK) {
		return syscall_error(SYSCALL_ERROR_BAD_FD);
	}

	if (listener->f_type != FILE_TYPE_SOCKET) {
		file_rele(listener);
		return syscall_error(SYSCALL_ERROR_INVALID_ARGUMENT);
	}

	socket_t socket;
	bool accepted = socket_accept(listener->f_socket, true, &socket);

	file_rele(listener);

	if (!accepted) return syscall_error(SYSCALL_ERROR_IO);

	return syscall_socket_install(socket);
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
	case SYSCALL_IPC_PORT_ALLOCATE: return syscall_ipc_port_allocate();
	case SYSCALL_IPC_PORT_DEALLOCATE: return syscall_ipc_port_deallocate(request->arguments[0]);
	case SYSCALL_IPC_BOOTSTRAP_PORT: return syscall_ipc_bootstrap_port();
	case SYSCALL_IPC_SEND: return syscall_ipc_send(request->arguments[0], request->arguments[1], request->arguments[2], request->arguments[3]);
	case SYSCALL_IPC_RECEIVE: return syscall_ipc_receive(request->arguments[0], request->arguments[1], request->arguments[2], request->arguments[3], request->arguments[4]);
	case SYSCALL_IPC_REGISTER_BOOTSTRAP: return syscall_ipc_register_bootstrap(request->arguments[0]);
	case SYSCALL_DISPLAY_CLAIM: return syscall_display_claim();
	case SYSCALL_SHM_CREATE: return syscall_shm_create(request->arguments[0]);
	case SYSCALL_SHM_MAP: return syscall_shm_map(request->arguments[0]);
	case SYSCALL_MMAP: return syscall_mmap(request->arguments[0], request->arguments[1]);
	case SYSCALL_MUNMAP: return syscall_munmap(request->arguments[0], request->arguments[1]);
	case SYSCALL_THREAD_CREATE: return syscall_thread_create(request->arguments[0], request->arguments[1], request->arguments[2]);
	case SYSCALL_THREAD_EXIT: return syscall_thread_exit(request->arguments[0]);
	case SYSCALL_THREAD_SELF: return syscall_thread_self();
	case SYSCALL_SOCKET_LISTEN: return syscall_socket_listen(request->arguments[0], request->arguments[1]);
	case SYSCALL_SOCKET_CONNECT: return syscall_socket_connect(request->arguments[0]);
	case SYSCALL_SOCKET_ACCEPT: return syscall_socket_accept(request->arguments[0]);
	case SYSCALL_FORK: return syscall_fork();
	case SYSCALL_EXEC: return syscall_exec(request->arguments[0], request->arguments[1]);
	case SYSCALL_KILL: return syscall_kill(request->arguments[0], request->arguments[1]);
	case SYSCALL_SIGACTION: return syscall_sigaction(request->arguments[0], request->arguments[1], request->arguments[2]);
	case SYSCALL_SIGPROCMASK: return syscall_sigprocmask(request->arguments[0], request->arguments[1]);
	case SYSCALL_SIGRETURN: return syscall_sigreturn();
	case SYSCALL_GETPPID: return syscall_getppid();
	case SYSCALL_WAIT: return syscall_wait(request->arguments[0], request->arguments[1]);
	case SYSCALL_IPC_RECEIVE_WAIT: return syscall_ipc_receive_wait(request->arguments[0], request->arguments[1], request->arguments[2], request->arguments[3], request->arguments[4]);
	default: return syscall_error(SYSCALL_ERROR_UNKNOWN);
	}
}
