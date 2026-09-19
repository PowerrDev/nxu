#ifndef NXU_USER_SYSCALL_H
#define NXU_USER_SYSCALL_H

#include <kern/syscall/syscall_defs.h>

#include <stdint.h>

int64_t nxu_exit(uint64_t status);
int64_t nxu_write(uint64_t descriptor, const void *buffer, uint64_t length);
int64_t nxu_get_version(void *buffer, uint64_t capacity);
int64_t nxu_open(const char *path, uint64_t flags);
int64_t nxu_read(uint64_t descriptor, void *buffer, uint64_t length);
int64_t nxu_close(uint64_t descriptor);
int64_t nxu_spawn(const char *path, const char *name);
int64_t nxu_waitpid(uint64_t pid, uint64_t *status);
int64_t nxu_getpid(void);
int64_t nxu_yield(void);
int64_t nxu_get_boot_args(char *buffer, uint64_t capacity);
int64_t nxu_klog_read(uint64_t *cursor, char *buffer, uint64_t capacity);
int64_t nxu_unlink(const char *path);
int64_t nxu_sync(void);
int64_t nxu_uptime_us(void);
int64_t nxu_readdir(uint64_t descriptor, nxu_dirent_t *entry);
int64_t nxu_seek(uint64_t descriptor, uint64_t offset);
int64_t nxu_stat(const char *path, nxu_stat_t *stat);
int64_t nxu_mkdir(const char *path);
int64_t nxu_recovery_fs_mount_info(uint32_t index, nxu_recovery_fs_mount_info_t *info);
int64_t nxu_recovery_fs_mount(const char *filesystem, uint32_t device_index, const char *path);
int64_t nxu_recovery_fs_unmount(const char *path);
int64_t nxu_recovery_block_info(uint32_t index, nxu_recovery_block_info_t *info);
int64_t nxu_recovery_fs_space_info(const char *path, nxu_recovery_fs_space_info_t *info);
int64_t nxu_recovery_block_layout_info(uint32_t index, nxu_recovery_block_layout_info_t *info);
int64_t nxu_recovery_block_partition_info(uint32_t device_index, uint32_t partition_index, nxu_recovery_block_partition_info_t *info);
int64_t nxu_recovery_block_health_info(uint32_t device_index, nxu_recovery_block_health_info_t *info);
int64_t nxu_recovery_block_verify(uint32_t device_index);
int64_t nxu_recovery_display_info(nxu_recovery_display_info_t *info);
int64_t nxu_recovery_present(const uint32_t *pixels, uint32_t stride, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
int64_t nxu_recovery_input(nxu_recovery_input_event_t *event);
int64_t nxu_system_reset(void);

#endif
