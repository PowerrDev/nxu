#include <Recovery/RecoveryServices.h>

#include <nxu/string.h>

#include <stdint.h>

_Static_assert(sizeof(recovery_fs_mount_info_t) == sizeof(nxu_recovery_fs_mount_info_t), "recovery mount ABI mismatch");
_Static_assert(sizeof(recovery_fs_space_info_t) == sizeof(nxu_recovery_fs_space_info_t), "recovery space ABI mismatch");
_Static_assert(sizeof(recovery_block_info_t) == sizeof(nxu_recovery_block_info_t), "recovery block ABI mismatch");
_Static_assert(sizeof(recovery_block_partition_info_t) == sizeof(nxu_recovery_block_partition_info_t), "recovery partition ABI mismatch");
_Static_assert(sizeof(recovery_block_health_info_t) == sizeof(nxu_recovery_block_health_info_t), "recovery health ABI mismatch");

static int64_t recovery_not_supported(void)
{
	return -(int64_t)NXU_SYS_E_NOT_SUPPORTED;
}

static void recovery_copy_memory(void *destination, const void *source, uint64_t size)
{
	uint8_t *destination_bytes = destination;
	const uint8_t *source_bytes = source;
	for (uint64_t index = 0ULL; index < size; index++) destination_bytes[index] = source_bytes[index];
}

int64_t recovery_write_string(const char *string)
{
	return nxu_write(1ULL, string, nxu_strlen(string));
}

void recovery_sched_wait(void)
{
	(void)nxu_yield();
}

int64_t recovery_system_reset(void)
{
	return nxu_system_reset();
}

const char *recovery_error_name(int64_t error)
{
	if (error >= 0) return "success";

	switch (-error) {
	case NXU_SYS_E_INVALID_ARGUMENT: return "invalid argument";
	case NXU_SYS_E_BAD_ADDRESS: return "bad address";
	case NXU_SYS_E_NOT_FOUND: return "not found";
	case NXU_SYS_E_IO: return "I/O error";
	case NXU_SYS_E_NO_MEMORY: return "out of memory";
	case NXU_SYS_E_NO_SPACE: return "no space";
	case NXU_SYS_E_NOT_SUPPORTED: return "not supported";
	case NXU_SYS_E_AGAIN: return "try again";
	case NXU_SYS_E_BAD_FD: return "bad file descriptor";
	case NXU_SYS_E_EXISTS: return "already exists";
	case NXU_SYS_E_BUSY: return "busy";
	default: return "unknown error";
	}
}

int64_t recovery_get_version(char *buffer, uint64_t capacity)
{
	return nxu_get_version(buffer, capacity);
}

int64_t recovery_fs_stat(const char *path, recovery_fs_stat_t *stat)
{
	if (path == 0 || stat == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	nxu_stat_t system_stat;
	int64_t result = nxu_stat(path, &system_stat);
	if (result < 0) return result;

	stat->id = system_stat.inode;
	stat->size = system_stat.size;
	stat->type = system_stat.type;
	stat->reserved = 0U;
	return 0;
}

int64_t recovery_fs_readdir(const char *path, uint32_t index, recovery_fs_dirent_t *entry)
{
	if (path == 0 || entry == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor < 0) return descriptor;

	nxu_dirent_t current;
	int64_t result = 0;
	for (uint32_t current_index = 0U; current_index <= index; current_index++) {
		result = nxu_readdir((uint64_t)descriptor, &current);
		if (result <= 0) break;
	}

	(void)nxu_close((uint64_t)descriptor);
	if (result <= 0) return result == 0 ? -(int64_t)NXU_SYS_E_NOT_FOUND : result;

	entry->id = current.inode;
	entry->size = 0ULL;
	entry->type = current.type == NXU_DIRENT_TYPE_DIRECTORY ? RECOVERY_FS_TYPE_DIRECTORY : RECOVERY_FS_TYPE_FILE;
	entry->reserved = 0U;
	uint32_t length = current.name_length;
	if (length > RECOVERY_FS_NAME_MAX) length = RECOVERY_FS_NAME_MAX;
	for (uint32_t offset = 0U; offset < length; offset++) entry->name[offset] = current.name[offset];
	entry->name[length] = '\0';
	return 0;
}

int64_t recovery_fs_read(const char *path, uint64_t offset, void *buffer, uint32_t capacity)
{
	int64_t descriptor = nxu_open(path, NXU_O_READ);
	if (descriptor < 0) return descriptor;
	int64_t seek_result = nxu_seek((uint64_t)descriptor, offset);
	if (seek_result < 0) {
		(void)nxu_close((uint64_t)descriptor);
		return seek_result;
	}

	int64_t result = nxu_read((uint64_t)descriptor, buffer, capacity);
	(void)nxu_close((uint64_t)descriptor);
	return result;
}

int64_t recovery_fs_mkdir(const char *path)
{
	return nxu_mkdir(path);
}

int64_t recovery_fs_touch(const char *path)
{
	int64_t descriptor = nxu_open(path, NXU_O_WRITE | NXU_O_CREATE);
	if (descriptor < 0) return descriptor;
	return nxu_close((uint64_t)descriptor);
}

int64_t recovery_fs_write(const char *path, const void *data, uint32_t size)
{
	int64_t descriptor = nxu_open(path, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (descriptor < 0) return descriptor;
	int64_t result = nxu_write((uint64_t)descriptor, data, size);
	(void)nxu_close((uint64_t)descriptor);
	return result;
}

int64_t recovery_fs_unlink(const char *path) { return nxu_unlink(path); }
int64_t recovery_fs_rmdir(const char *path) { (void)path; return recovery_not_supported(); }
int64_t recovery_fs_sync(void) { return nxu_sync(); }
int64_t recovery_fs_mount_info(uint32_t index, recovery_fs_mount_info_t *info)
{
	if (info == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	nxu_recovery_fs_mount_info_t system_info;
	int64_t result = nxu_recovery_fs_mount_info(index, &system_info);
	if (result < 0) return result;

	recovery_copy_memory(info, &system_info, sizeof(*info));
	return 0;
}

int64_t recovery_fs_mount(const char *filesystem, uint32_t device_index, const char *path)
{
	if (filesystem == 0 || path == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	return nxu_recovery_fs_mount(filesystem, device_index, path);
}

int64_t recovery_fs_unmount(const char *path)
{
	if (path == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	return nxu_recovery_fs_unmount(path);
}
int64_t recovery_fs_copy(const char *source, const char *destination)
{
	if (source == 0 || destination == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	int64_t input = nxu_open(source, NXU_O_READ);
	if (input < 0) return input;
	int64_t output = nxu_open(destination, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
	if (output < 0) {
		(void)nxu_close((uint64_t)input);
		return output;
	}

	uint8_t buffer[RECOVERY_FS_READ_MAX];
	int64_t result = 0;
	for (;;) {
		int64_t read_size = nxu_read((uint64_t)input, buffer, sizeof(buffer));
		if (read_size < 0) { result = read_size; break; }
		if (read_size == 0) break;
		int64_t written = nxu_write((uint64_t)output, buffer, (uint64_t)read_size);
		if (written != read_size) {
			result = written < 0 ? written : -(int64_t)NXU_SYS_E_IO;
			break;
		}
	}

	(void)nxu_close((uint64_t)output);
	(void)nxu_close((uint64_t)input);
	return result;
}
int64_t recovery_block_info(uint32_t index, recovery_block_info_t *info)
{
	if (info == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	nxu_recovery_block_info_t system_info;
	int64_t result = nxu_recovery_block_info(index, &system_info);
	if (result < 0) return result;
	recovery_copy_memory(info, &system_info, sizeof(*info));
	return 0;
}

int64_t recovery_fs_space_info(const char *path, recovery_fs_space_info_t *info)
{
	if (path == 0 || info == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	nxu_recovery_fs_space_info_t system_info;
	int64_t result = nxu_recovery_fs_space_info(path, &system_info);
	if (result < 0) return result;
	recovery_copy_memory(info, &system_info, sizeof(*info));
	return 0;
}

int64_t recovery_block_layout_info(uint32_t index, recovery_block_layout_info_t *info)
{
	if (info == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	nxu_recovery_block_layout_info_t system_info;
	int64_t result = nxu_recovery_block_layout_info(index, &system_info);
	if (result < 0) return result;
	info->scheme = system_info.scheme;
	info->partition_count = system_info.partition_count;
	return 0;
}

int64_t recovery_block_partition_info(uint32_t device_index, uint32_t partition_index, recovery_block_partition_info_t *info)
{
	if (info == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	nxu_recovery_block_partition_info_t system_info;
	int64_t result = nxu_recovery_block_partition_info(device_index, partition_index, &system_info);
	if (result < 0) return result;
	recovery_copy_memory(info, &system_info, sizeof(*info));
	return 0;
}

int64_t recovery_block_health_info(uint32_t device_index, recovery_block_health_info_t *info)
{
	if (info == 0) return -(int64_t)NXU_SYS_E_INVALID_ARGUMENT;
	nxu_recovery_block_health_info_t system_info;
	int64_t result = nxu_recovery_block_health_info(device_index, &system_info);
	if (result < 0) return result;
	recovery_copy_memory(info, &system_info, sizeof(*info));
	return 0;
}
int64_t recovery_block_gpt_initialize(uint32_t device_index) { (void)device_index; return recovery_not_supported(); }
int64_t recovery_block_partition_create(uint32_t device_index, uint64_t sector_count, const char *name, recovery_block_partition_role_t role, recovery_block_partition_info_t *info) { (void)device_index; (void)sector_count; (void)role; (void)name; (void)info; return recovery_not_supported(); }
int64_t recovery_block_partition_delete(uint32_t device_index, uint32_t partition_index) { (void)device_index; (void)partition_index; return recovery_not_supported(); }
int64_t recovery_block_verify(uint32_t device_index) { return nxu_recovery_block_verify(device_index); }
