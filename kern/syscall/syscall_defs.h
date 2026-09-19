#ifndef NXU_ABI_SYSCALL_H
#define NXU_ABI_SYSCALL_H

#include <stdint.h>

#define NXU_SYS_EXIT 1ULL
#define NXU_SYS_WRITE 3ULL
#define NXU_SYS_GET_VERSION 4ULL
#define NXU_SYS_OPEN 5ULL
#define NXU_SYS_READ 6ULL
#define NXU_SYS_CLOSE 7ULL
#define NXU_SYS_SPAWN 8ULL
#define NXU_SYS_WAITPID 9ULL
#define NXU_SYS_GETPID 10ULL
#define NXU_SYS_YIELD 11ULL
#define NXU_SYS_GET_BOOT_ARGS 12ULL
#define NXU_SYS_KLOG_READ 13ULL
#define NXU_SYS_UNLINK 14ULL
#define NXU_SYS_SYNC 15ULL
#define NXU_SYS_UPTIME_US 16ULL
#define NXU_SYS_READDIR 17ULL
#define NXU_SYS_RECOVERY_DISPLAY_INFO 18ULL
#define NXU_SYS_RECOVERY_PRESENT 19ULL
#define NXU_SYS_RECOVERY_INPUT 20ULL
#define NXU_SYS_SYSTEM_RESET 21ULL
#define NXU_SYS_SEEK 22ULL
#define NXU_SYS_STAT 23ULL
#define NXU_SYS_MKDIR 24ULL
#define NXU_SYS_RECOVERY_FS_MOUNT_INFO 25ULL
#define NXU_SYS_RECOVERY_FS_MOUNT 26ULL
#define NXU_SYS_RECOVERY_FS_UNMOUNT 27ULL
#define NXU_SYS_RECOVERY_BLOCK_INFO 28ULL
#define NXU_SYS_RECOVERY_FS_SPACE_INFO 29ULL
#define NXU_SYS_RECOVERY_BLOCK_LAYOUT_INFO 30ULL
#define NXU_SYS_RECOVERY_BLOCK_PARTITION_INFO 31ULL
#define NXU_SYS_RECOVERY_BLOCK_HEALTH_INFO 32ULL
#define NXU_SYS_RECOVERY_BLOCK_VERIFY 33ULL

#define NXU_SYS_E_UNKNOWN 1LL
#define NXU_SYS_E_INVALID_ARGUMENT 2LL
#define NXU_SYS_E_BAD_ADDRESS 3LL
#define NXU_SYS_E_NOT_FOUND 4LL
#define NXU_SYS_E_IO 5LL
#define NXU_SYS_E_NO_MEMORY 6LL
#define NXU_SYS_E_NO_SPACE 7LL
#define NXU_SYS_E_NOT_SUPPORTED 8LL
#define NXU_SYS_E_AGAIN 9LL
#define NXU_SYS_E_BAD_FD 10LL
#define NXU_SYS_E_EXISTS 11LL
#define NXU_SYS_E_BUSY 12LL

#define NXU_O_READ (1U << 0U)
#define NXU_O_WRITE (1U << 1U)
#define NXU_O_CREATE (1U << 2U)
#define NXU_O_TRUNCATE (1U << 3U)
#define NXU_O_APPEND (1U << 4U)

#define NXU_DIRENT_NAME_MAX 255U

#define NXU_DIRENT_TYPE_UNKNOWN 0U
#define NXU_DIRENT_TYPE_REGULAR 1U
#define NXU_DIRENT_TYPE_DIRECTORY 2U


#define NXU_RECOVERY_INPUT_NONE 0U
#define NXU_RECOVERY_INPUT_KEY 1U
#define NXU_RECOVERY_INPUT_POINTER 2U

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t pixel_format;
} nxu_recovery_display_info_t;

typedef struct {
	uint32_t kind;
	uint32_t code;
	int32_t value;
	int32_t x;
	int32_t y;
	uint32_t buttons;
	uint32_t modifiers;
} nxu_recovery_input_event_t;

#define NXU_RECOVERY_FS_PATH_MAX 256U
#define NXU_RECOVERY_FS_TYPE_MAX 31U
#define NXU_RECOVERY_FS_DEVICE_MAX 31U
#define NXU_RECOVERY_BLOCK_PARTITION_NAME_MAX 35U
#define NXU_RECOVERY_FS_NO_DEVICE UINT32_MAX

#define NXU_RECOVERY_BLOCK_LAYOUT_RAW 0U
#define NXU_RECOVERY_BLOCK_LAYOUT_MBR 1U
#define NXU_RECOVERY_BLOCK_LAYOUT_GPT 2U

typedef struct {
	char filesystem[NXU_RECOVERY_FS_TYPE_MAX + 1U];
	char path[NXU_RECOVERY_FS_PATH_MAX];
	char device[NXU_RECOVERY_FS_DEVICE_MAX + 1U];
	uint32_t device_index;
	uint32_t read_only;
} nxu_recovery_fs_mount_info_t;

typedef struct {
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint32_t block_size;
	uint32_t read_only;
} nxu_recovery_fs_space_info_t;

typedef struct {
	uint32_t scheme;
	uint32_t partition_count;
} nxu_recovery_block_layout_info_t;

typedef struct {
	uint64_t start_sector;
	uint64_t sector_count;
	uint32_t scheme;
	uint32_t index;
	uint32_t type;
	uint32_t reserved;
	char name[NXU_RECOVERY_BLOCK_PARTITION_NAME_MAX + 1U];
} nxu_recovery_block_partition_info_t;

typedef struct {
	char name[NXU_RECOVERY_FS_DEVICE_MAX + 1U];
	uint64_t sector_count;
	uint32_t sector_size;
	uint32_t logical_block_size;
	uint32_t read_only;
	uint32_t reserved;
} nxu_recovery_block_info_t;

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
} nxu_recovery_block_health_info_t;

typedef struct {
	uint64_t inode;
	uint32_t type;
	uint32_t name_length;
	char name[NXU_DIRENT_NAME_MAX + 1U];
} nxu_dirent_t;

#define NXU_STAT_TYPE_UNKNOWN 0U
#define NXU_STAT_TYPE_REGULAR 1U
#define NXU_STAT_TYPE_DIRECTORY 2U
#define NXU_STAT_TYPE_CHARACTER 3U
#define NXU_STAT_TYPE_BLOCK 4U

typedef struct {
	uint64_t inode;
	uint64_t size;
	uint32_t type;
	uint32_t reserved;
} nxu_stat_t;

#endif
