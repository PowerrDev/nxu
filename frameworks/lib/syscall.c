#include <nxu/syscall.h>

static int64_t
nxu_syscall0(uint64_t number)
{
	register uint64_t x8 __asm__("x8") = number;
	register uint64_t x0 __asm__("x0");
	__asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
	return (int64_t)x0;
}

static int64_t
nxu_syscall1(uint64_t number, uint64_t argument0)
{
	register uint64_t x8 __asm__("x8") = number;
	register uint64_t x0 __asm__("x0") = argument0;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return (int64_t)x0;
}

static int64_t
nxu_syscall2(uint64_t number, uint64_t argument0, uint64_t argument1)
{
	register uint64_t x8 __asm__("x8") = number;
	register uint64_t x0 __asm__("x0") = argument0;
	register uint64_t x1 __asm__("x1") = argument1;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
	return (int64_t)x0;
}

static int64_t
nxu_syscall3(uint64_t number, uint64_t argument0, uint64_t argument1, uint64_t argument2)
{
	register uint64_t x8 __asm__("x8") = number;
	register uint64_t x0 __asm__("x0") = argument0;
	register uint64_t x1 __asm__("x1") = argument1;
	register uint64_t x2 __asm__("x2") = argument2;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return (int64_t)x0;
}

static int64_t
nxu_syscall4(uint64_t number, uint64_t argument0, uint64_t argument1, uint64_t argument2, uint64_t argument3)
{
	register uint64_t x8 __asm__("x8") = number;
	register uint64_t x0 __asm__("x0") = argument0;
	register uint64_t x1 __asm__("x1") = argument1;
	register uint64_t x2 __asm__("x2") = argument2;
	register uint64_t x3 __asm__("x3") = argument3;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
	return (int64_t)x0;
}

static int64_t
nxu_syscall5(uint64_t number, uint64_t argument0, uint64_t argument1, uint64_t argument2, uint64_t argument3, uint64_t argument4)
{
	register uint64_t x8 __asm__("x8") = number;
	register uint64_t x0 __asm__("x0") = argument0;
	register uint64_t x1 __asm__("x1") = argument1;
	register uint64_t x2 __asm__("x2") = argument2;
	register uint64_t x3 __asm__("x3") = argument3;
	register uint64_t x4 __asm__("x4") = argument4;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
	return (int64_t)x0;
}

static int64_t
nxu_syscall6(uint64_t number, uint64_t argument0, uint64_t argument1, uint64_t argument2, uint64_t argument3, uint64_t argument4, uint64_t argument5)
{
	register uint64_t x8 __asm__("x8") = number;
	register uint64_t x0 __asm__("x0") = argument0;
	register uint64_t x1 __asm__("x1") = argument1;
	register uint64_t x2 __asm__("x2") = argument2;
	register uint64_t x3 __asm__("x3") = argument3;
	register uint64_t x4 __asm__("x4") = argument4;
	register uint64_t x5 __asm__("x5") = argument5;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
	return (int64_t)x0;
}

int64_t nxu_exit(uint64_t status) { return nxu_syscall1(NXU_SYS_EXIT, status); }
int64_t nxu_write(uint64_t descriptor, const void *buffer, uint64_t length) { return nxu_syscall3(NXU_SYS_WRITE, descriptor, (uint64_t)buffer, length); }
int64_t nxu_get_version(void *buffer, uint64_t capacity) { return nxu_syscall2(NXU_SYS_GET_VERSION, (uint64_t)buffer, capacity); }
int64_t nxu_open(const char *path, uint64_t flags) { return nxu_syscall2(NXU_SYS_OPEN, (uint64_t)path, flags); }
int64_t nxu_read(uint64_t descriptor, void *buffer, uint64_t length) { return nxu_syscall3(NXU_SYS_READ, descriptor, (uint64_t)buffer, length); }
int64_t nxu_close(uint64_t descriptor) { return nxu_syscall1(NXU_SYS_CLOSE, descriptor); }
int64_t nxu_spawn(const char *path, const char *name) { return nxu_syscall2(NXU_SYS_SPAWN, (uint64_t)path, (uint64_t)name); }
int64_t nxu_waitpid(uint64_t pid, uint64_t *status) { return nxu_syscall2(NXU_SYS_WAITPID, pid, (uint64_t)status); }
int64_t nxu_getpid(void) { return nxu_syscall0(NXU_SYS_GETPID); }
int64_t nxu_yield(void) { return nxu_syscall0(NXU_SYS_YIELD); }
int64_t nxu_get_boot_args(char *buffer, uint64_t capacity) { return nxu_syscall2(NXU_SYS_GET_BOOT_ARGS, (uint64_t)buffer, capacity); }
int64_t nxu_klog_read(uint64_t *cursor, char *buffer, uint64_t capacity) { return nxu_syscall3(NXU_SYS_KLOG_READ, (uint64_t)cursor, (uint64_t)buffer, capacity); }
int64_t nxu_unlink(const char *path) { return nxu_syscall1(NXU_SYS_UNLINK, (uint64_t)path); }
int64_t nxu_sync(void) { return nxu_syscall0(NXU_SYS_SYNC); }
int64_t nxu_uptime_us(void) { return nxu_syscall0(NXU_SYS_UPTIME_US); }
int64_t nxu_readdir(uint64_t descriptor, nxu_dirent_t *entry) { return nxu_syscall2(NXU_SYS_READDIR, descriptor, (uint64_t)entry); }
int64_t nxu_seek(uint64_t descriptor, uint64_t offset) { return nxu_syscall2(NXU_SYS_SEEK, descriptor, offset); }
int64_t nxu_stat(const char *path, nxu_stat_t *stat) { return nxu_syscall2(NXU_SYS_STAT, (uint64_t)path, (uint64_t)stat); }
int64_t nxu_mkdir(const char *path) { return nxu_syscall1(NXU_SYS_MKDIR, (uint64_t)path); }
int64_t nxu_recovery_fs_mount_info(uint32_t index, nxu_recovery_fs_mount_info_t *info) { return nxu_syscall2(NXU_SYS_RECOVERY_FS_MOUNT_INFO, index, (uint64_t)info); }
int64_t nxu_recovery_fs_mount(const char *filesystem, uint32_t device_index, const char *path) { return nxu_syscall3(NXU_SYS_RECOVERY_FS_MOUNT, (uint64_t)filesystem, device_index, (uint64_t)path); }
int64_t nxu_recovery_fs_unmount(const char *path) { return nxu_syscall1(NXU_SYS_RECOVERY_FS_UNMOUNT, (uint64_t)path); }
int64_t nxu_recovery_block_info(uint32_t index, nxu_recovery_block_info_t *info) { return nxu_syscall2(NXU_SYS_RECOVERY_BLOCK_INFO, index, (uint64_t)info); }
int64_t nxu_recovery_fs_space_info(const char *path, nxu_recovery_fs_space_info_t *info) { return nxu_syscall2(NXU_SYS_RECOVERY_FS_SPACE_INFO, (uint64_t)path, (uint64_t)info); }
int64_t nxu_recovery_block_layout_info(uint32_t index, nxu_recovery_block_layout_info_t *info) { return nxu_syscall2(NXU_SYS_RECOVERY_BLOCK_LAYOUT_INFO, index, (uint64_t)info); }
int64_t nxu_recovery_block_partition_info(uint32_t device_index, uint32_t partition_index, nxu_recovery_block_partition_info_t *info) { return nxu_syscall3(NXU_SYS_RECOVERY_BLOCK_PARTITION_INFO, device_index, partition_index, (uint64_t)info); }
int64_t nxu_recovery_block_health_info(uint32_t device_index, nxu_recovery_block_health_info_t *info) { return nxu_syscall2(NXU_SYS_RECOVERY_BLOCK_HEALTH_INFO, device_index, (uint64_t)info); }
int64_t nxu_recovery_block_verify(uint32_t device_index) { return nxu_syscall1(NXU_SYS_RECOVERY_BLOCK_VERIFY, device_index); }

int64_t nxu_recovery_display_info(nxu_recovery_display_info_t *info) { return nxu_syscall1(NXU_SYS_RECOVERY_DISPLAY_INFO, (uint64_t)info); }
int64_t nxu_recovery_present(const uint32_t *pixels, uint32_t stride, uint32_t x, uint32_t y, uint32_t width, uint32_t height) { return nxu_syscall6(NXU_SYS_RECOVERY_PRESENT, (uint64_t)pixels, stride, x, y, width, height); }
int64_t nxu_recovery_input(nxu_recovery_input_event_t *event) { return nxu_syscall1(NXU_SYS_RECOVERY_INPUT, (uint64_t)event); }
int64_t nxu_system_reset(void) { return nxu_syscall0(NXU_SYS_SYSTEM_RESET); }

int64_t nxu_ipc_port_allocate(void) { return nxu_syscall0(NXU_SYS_IPC_PORT_ALLOCATE); }
int64_t nxu_ipc_port_deallocate(uint32_t name) { return nxu_syscall1(NXU_SYS_IPC_PORT_DEALLOCATE, name); }
int64_t nxu_ipc_bootstrap_port(void) { return nxu_syscall0(NXU_SYS_IPC_BOOTSTRAP_PORT); }
int64_t nxu_ipc_send(uint32_t dest_name, const void *buffer, uint64_t length, uint32_t xfer_name) { return nxu_syscall4(NXU_SYS_IPC_SEND, dest_name, (uint64_t)buffer, length, xfer_name); }
int64_t nxu_ipc_receive(uint32_t port_name, void *buffer, uint64_t capacity, uint32_t *out_xfer_name, uint32_t *out_xfer_type) { return nxu_syscall5(NXU_SYS_IPC_RECEIVE, port_name, (uint64_t)buffer, capacity, (uint64_t)out_xfer_name, (uint64_t)out_xfer_type); }
int64_t nxu_ipc_register_bootstrap(uint32_t port_name) { return nxu_syscall1(NXU_SYS_IPC_REGISTER_BOOTSTRAP, port_name); }
int64_t nxu_display_claim(void) { return nxu_syscall0(NXU_SYS_DISPLAY_CLAIM); }
int64_t nxu_shm_create(uint64_t size) { return nxu_syscall1(NXU_SYS_SHM_CREATE, size); }
int64_t nxu_shm_map(uint64_t id) { return nxu_syscall1(NXU_SYS_SHM_MAP, id); }
int64_t nxu_mmap(uint64_t size, uint64_t prot_flags) { return nxu_syscall2(NXU_SYS_MMAP, size, prot_flags); }
int64_t nxu_munmap(uint64_t address, uint64_t size) { return nxu_syscall2(NXU_SYS_MUNMAP, address, size); }
int64_t nxu_thread_create(void (*entry)(void *arg), void *stack, void *arg) { return nxu_syscall3(NXU_SYS_THREAD_CREATE, (uint64_t)entry, (uint64_t)stack, (uint64_t)arg); }
int64_t nxu_thread_exit(uint64_t status) { return nxu_syscall1(NXU_SYS_THREAD_EXIT, status); }
int64_t nxu_thread_self(void) { return nxu_syscall0(NXU_SYS_THREAD_SELF); }
int64_t nxu_socket_listen(const char *name, uint32_t backlog) { return nxu_syscall2(NXU_SYS_SOCKET_LISTEN, (uint64_t)name, backlog); }
int64_t nxu_socket_connect(const char *name) { return nxu_syscall1(NXU_SYS_SOCKET_CONNECT, (uint64_t)name); }
int64_t nxu_socket_accept(uint64_t listen_descriptor) { return nxu_syscall1(NXU_SYS_SOCKET_ACCEPT, listen_descriptor); }

int64_t nxu_fork(void) { return nxu_syscall0(NXU_SYS_FORK); }
int64_t nxu_exec(const char *path, const char *const *argv) { return nxu_syscall2(NXU_SYS_EXEC, (uint64_t)path, (uint64_t)argv); }
int64_t nxu_getppid(void) { return nxu_syscall0(NXU_SYS_GETPPID); }
int64_t nxu_kill(uint64_t pid, uint32_t signal) { return nxu_syscall2(NXU_SYS_KILL, pid, signal); }
int64_t nxu_sigaction(uint32_t signal, const nxu_sigaction_t *action, nxu_sigaction_t *old_action) { return nxu_syscall3(NXU_SYS_SIGACTION, signal, (uint64_t)action, (uint64_t)old_action); }
int64_t nxu_sigprocmask(uint32_t how, uint32_t set) { return nxu_syscall2(NXU_SYS_SIGPROCMASK, how, set); }

/*
 * The kernel points a handler's return address here. It is never called: the
 * handler's "ret" lands on it, and it hands the interrupted context back to
 * the kernel with sigreturn.
 */
__asm__(
	".text\n"
	".global nxu_sigreturn_trampoline\n"
	".type nxu_sigreturn_trampoline, %function\n"
	"nxu_sigreturn_trampoline:\n"
	"\tmov x8, #56\n"
	"\tsvc #0\n"
	".size nxu_sigreturn_trampoline, . - nxu_sigreturn_trampoline\n"
);

_Static_assert(NXU_SYS_SIGRETURN == 56ULL, "nxu_sigreturn_trampoline hard-codes the sigreturn number");

int64_t
nxu_signal(uint32_t signal, void (*handler)(int))
{
	nxu_sigaction_t action = {
		.handler = (uint64_t)handler,
		.restorer = (uint64_t)nxu_sigreturn_trampoline,
		.mask = 0U,
		.flags = 0U
	};

	return nxu_sigaction(signal, &action, 0);
}

int64_t nxu_wait(uint64_t pid, uint64_t *status) { return nxu_syscall2(NXU_SYS_WAIT, pid, (uint64_t)status); }
int64_t nxu_ipc_receive_wait(uint32_t port_name, void *buffer, uint64_t capacity, uint32_t *out_xfer_name, uint32_t *out_xfer_type) { return nxu_syscall5(NXU_SYS_IPC_RECEIVE_WAIT, port_name, (uint64_t)buffer, capacity, (uint64_t)out_xfer_name, (uint64_t)out_xfer_type); }
