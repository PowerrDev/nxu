#ifndef NXU_RECOVERY_SERVICES_H
#define NXU_RECOVERY_SERVICES_H

#include <kern/syscall/syscall_defs.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define RECOVERY_FS_PATH_MAX 256U
#define RECOVERY_FS_NAME_MAX 63U
#define RECOVERY_FS_TYPE_MAX 31U
#define RECOVERY_FS_DEVICE_MAX 31U
#define RECOVERY_BLOCK_PARTITION_NAME_MAX 35U
#define RECOVERY_FS_NO_DEVICE UINT32_MAX
#define RECOVERY_FS_READ_MAX 512U

#define RECOVERY_ERROR_INVALID_ARGUMENT NXU_SYS_E_INVALID_ARGUMENT
#define RECOVERY_ERROR_NOT_FOUND NXU_SYS_E_NOT_FOUND
#define RECOVERY_ERROR_IO NXU_SYS_E_IO
#define RECOVERY_ERROR_EXISTS NXU_SYS_E_EXISTS

#define RECOVERY_FS_TYPE_FILE 1U
#define RECOVERY_FS_TYPE_DIRECTORY 2U
#define RECOVERY_FS_TYPE_CHARACTER 3U
#define RECOVERY_FS_TYPE_BLOCK 4U

#define RECOVERY_O_READ NXU_O_READ
#define RECOVERY_O_WRITE NXU_O_WRITE
#define RECOVERY_O_CREATE NXU_O_CREATE
#define RECOVERY_O_TRUNCATE NXU_O_TRUNCATE
#define RECOVERY_O_APPEND NXU_O_APPEND

#define RECOVERY_BLOCK_LAYOUT_RAW 0U
#define RECOVERY_BLOCK_LAYOUT_MBR 1U
#define RECOVERY_BLOCK_LAYOUT_GPT 2U

typedef enum {
	RECOVERY_BLOCK_PARTITION_ROLE_DATA = 0,
	RECOVERY_BLOCK_PARTITION_ROLE_SYSTEM = 1,
	RECOVERY_BLOCK_PARTITION_ROLE_EFI = 2
} recovery_block_partition_role_t;

typedef struct {
	uint64_t id;
	uint64_t size;
	uint32_t type;
	uint32_t reserved;
} recovery_fs_stat_t;

typedef struct {
	uint64_t id;
	uint64_t size;
	uint32_t type;
	uint32_t reserved;
	char name[RECOVERY_FS_NAME_MAX + 1U];
} recovery_fs_dirent_t;

typedef struct {
	char filesystem[RECOVERY_FS_TYPE_MAX + 1U];
	char path[RECOVERY_FS_PATH_MAX];
	char device[RECOVERY_FS_DEVICE_MAX + 1U];
	uint32_t device_index;
	uint32_t read_only;
} recovery_fs_mount_info_t;

typedef struct {
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint32_t block_size;
	uint32_t read_only;
} recovery_fs_space_info_t;

typedef struct {
	uint32_t scheme;
	uint32_t partition_count;
} recovery_block_layout_info_t;

typedef struct {
	uint64_t start_sector;
	uint64_t sector_count;
	uint32_t scheme;
	uint32_t index;
	uint32_t type;
	uint32_t reserved;
	char name[RECOVERY_BLOCK_PARTITION_NAME_MAX + 1U];
} recovery_block_partition_info_t;

typedef struct {
	char name[RECOVERY_FS_DEVICE_MAX + 1U];
	uint64_t sector_count;
	uint32_t sector_size;
	uint32_t logical_block_size;
	uint32_t read_only;
	uint32_t reserved;
} recovery_block_info_t;

typedef struct {
	uint64_t read_operations;
	uint64_t write_operations;
	uint64_t flush_operations;
	uint64_t read_errors;
	uint64_t write_errors;
	uint64_t flush_errors;
	uint32_t online;
	uint32_t healthy;
	uint32_t read_only;
	uint32_t flush_supported;
} recovery_block_health_info_t;

int64_t recovery_write_string(const char *string);
void recovery_sched_wait(void);
int64_t recovery_system_reset(void);
const char *recovery_error_name(int64_t error);
int64_t recovery_get_version(char *buffer, uint64_t capacity);
int64_t recovery_fs_stat(const char *path, recovery_fs_stat_t *stat);
int64_t recovery_fs_readdir(const char *path, uint32_t index, recovery_fs_dirent_t *entry);
int64_t recovery_fs_read(const char *path, uint64_t offset, void *buffer, uint32_t capacity);
int64_t recovery_fs_mkdir(const char *path);
int64_t recovery_fs_touch(const char *path);
int64_t recovery_fs_write(const char *path, const void *data, uint32_t size);
int64_t recovery_fs_unlink(const char *path);
int64_t recovery_fs_rmdir(const char *path);
int64_t recovery_fs_sync(void);
int64_t recovery_fs_mount_info(uint32_t index, recovery_fs_mount_info_t *info);
int64_t recovery_fs_mount(const char *filesystem, uint32_t device_index, const char *path);
int64_t recovery_fs_unmount(const char *path);
int64_t recovery_fs_copy(const char *source, const char *destination);
int64_t recovery_block_info(uint32_t index, recovery_block_info_t *info);
int64_t recovery_fs_space_info(const char *path, recovery_fs_space_info_t *info);
int64_t recovery_block_layout_info(uint32_t index, recovery_block_layout_info_t *info);
int64_t recovery_block_partition_info(uint32_t device_index, uint32_t partition_index, recovery_block_partition_info_t *info);
int64_t recovery_block_health_info(uint32_t device_index, recovery_block_health_info_t *info);
int64_t recovery_block_gpt_initialize(uint32_t device_index);
int64_t recovery_block_partition_create(uint32_t device_index, uint64_t sector_count, const char *name, recovery_block_partition_role_t role, recovery_block_partition_info_t *info);
int64_t recovery_block_partition_delete(uint32_t device_index, uint32_t partition_index);
int64_t recovery_block_verify(uint32_t device_index);

#endif
