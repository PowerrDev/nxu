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

int64_t nxu_ipc_port_allocate(void);
int64_t nxu_ipc_port_deallocate(uint32_t name);
int64_t nxu_ipc_bootstrap_port(void);
int64_t nxu_ipc_send(uint32_t dest_name, const void *buffer, uint64_t length, uint32_t xfer_name);

/*
 * Returns the received message's size on success, or a negative -NXU_SYS_E_AGAIN
 * when port_name's queue is empty -- poll this in an nxu_yield() loop, the
 * same pattern bootd already uses for nxu_waitpid. out_xfer_name/
 * out_xfer_type may be NULL to ignore a transfer.
 */
int64_t nxu_ipc_receive(uint32_t port_name, void *buffer, uint64_t capacity, uint32_t *out_xfer_name, uint32_t *out_xfer_type);

int64_t nxu_ipc_register_bootstrap(uint32_t port_name);

/* First-come-first-served claim on NXU_SYS_RECOVERY_PRESENT/_DISPLAY_INFO/
 * _INPUT outside triageOS/recovery boot -- see kern/console/display_owner.h. */
int64_t nxu_display_claim(void);

/* Returns a new shared-memory region's id (> 0), shareable as plain data in
 * an NXPC message; nxu_shm_map attaches to it from any process that learns
 * the id. See kern/ipc/shm_registry.h. */
int64_t nxu_shm_create(uint64_t size);
int64_t nxu_shm_map(uint64_t id);

/*
 * General-purpose private anonymous memory (heap growth, thread stacks) --
 * unlike nxu_shm_create/_map, this is not shareable with another process.
 * prot_flags is one of NXU_MMAP_PROT_READ_WRITE/READ_ONLY/READ_EXECUTE.
 * Returns the mapping's base VA on success. nxu_munmap's address/size must
 * exactly cover, or fall entirely within, one still-live nxu_mmap call --
 * see vm/vm_map.h.
 */
int64_t nxu_mmap(uint64_t size, uint64_t prot_flags);
int64_t nxu_munmap(uint64_t address, uint64_t size);

/*
 * pthread_create-style thread spawn sharing the caller's address space.
 * entry runs with arg in its initial argument register; stack is the new
 * thread's initial stack pointer (one past the last valid byte of a region
 * the caller itself nxu_mmap'd -- e.g. base + size from a prior nxu_mmap
 * call). Returns the new thread's id (> 0) on success.
 */
int64_t nxu_thread_create(void (*entry)(void *arg), void *stack, void *arg);

/*
 * Terminates only the calling thread if other threads remain active in this
 * task; equivalent to nxu_exit if it is the last one. Never returns.
 */
int64_t nxu_thread_exit(uint64_t status);

int64_t nxu_thread_self(void);

/*
 * Streaming, blocking, arbitrary-length local sockets (kern/ipc/socket.h)
 * -- unlike NXPC (nxu_ipc_send/receive), not bounded to 4096 bytes per
 * message and not poll-based. All three return an ordinary fd: read/write/
 * close it with nxu_read/nxu_write/nxu_close exactly like a regular file.
 */
int64_t nxu_socket_listen(const char *name, uint32_t backlog);
int64_t nxu_socket_connect(const char *name);
int64_t nxu_socket_accept(uint64_t listen_descriptor);

#endif
