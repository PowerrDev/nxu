/*
 * i386 system call wrappers: the counterpart of frameworks/lib/syscall.c.
 * The public API (frameworks/include/nxu/syscall.h) is unchanged, so no
 * caller needs to know which architecture it was built for.
 *
 * Convention (fixed by doc/i386/userland.md, implemented kernel-side by the
 * threads area):
 *
 *   trap          int $0x80
 *   number        eax (NXU_SYS_*)
 *   arguments     ebx, ecx, edx, esi, edi, ebp, in the order of arm64's
 *                 x0..x5 (six at most)
 *   result        eax, a signed 32-bit value; negative is -NXU_SYS_E_*
 *   preserved     every register except eax
 *
 * The public prototypes keep their 64-bit parameter and result types, so
 * this file narrows at the boundary:
 *
 *   arguments   a value that arm64 passes in a 64-bit register but that is
 *               a count, size, descriptor, id or file offset on a 32-bit
 *               machine is rejected with -NXU_SYS_E_INVALID_ARGUMENT when it
 *               does not fit in 32 bits, instead of being silently truncated.
 *               Sizes and lengths that also feed a result (read, write, ipc,
 *               ...) must fit in 31 bits, since a result above INT32_MAX
 *               would read as an error. Pointers are 32 bits by construction.
 *   results     the syscall result is sign-extended. The exceptions are
 *               nxu_mmap and nxu_shm_map, which return a user address that
 *               may be above INT32_MAX (the user window runs to
 *               0xBFFFFFFF): they are zero-extended unless they fall in the
 *               error range -4095..-1. nxu_uptime_us is described below.
 *   memory      out-parameters that are 64 bits on arm64 (the waitpid status,
 *               the klog cursor, the 64-bit fields of nxu_stat_t,
 *               nxu_dirent_t and the recovery info structs) stay 64-bit
 *               objects in memory and the kernel reads or writes all eight
 *               bytes, little-endian.
 *
 * NXU_SYS_UPTIME_US returns the low 32 bits of the microsecond counter in
 * eax, which wraps every 71.6 minutes and never fails. nxu_uptime_us()
 * widens that to a monotonic 64-bit value by noticing each wrap; it needs to
 * be called at least once per wrap period, which every caller (a poll loop)
 * does.
 */

#include <nxu/syscall.h>

#if !defined(__i386__)
#error "frameworks/lib/syscall_i386.c is the i386 implementation of the NXU system call wrappers"
#endif

#define NXU_ERROR_RANGE 4096U

static int32_t
nxu_syscall0(uint32_t number)
{
	int32_t result;
	__asm__ volatile("int $0x80" : "=a"(result) : "a"(number) : "memory");
	return result;
}

static int32_t
nxu_syscall1(uint32_t number, uint32_t argument0)
{
	int32_t result;
	__asm__ volatile("int $0x80" : "=a"(result) : "a"(number), "b"(argument0) : "memory");
	return result;
}

static int32_t
nxu_syscall2(uint32_t number, uint32_t argument0, uint32_t argument1)
{
	int32_t result;
	__asm__ volatile("int $0x80" : "=a"(result) : "a"(number), "b"(argument0), "c"(argument1) : "memory");
	return result;
}

static int32_t
nxu_syscall3(uint32_t number, uint32_t argument0, uint32_t argument1, uint32_t argument2)
{
	int32_t result;
	__asm__ volatile("int $0x80" : "=a"(result) : "a"(number), "b"(argument0), "c"(argument1), "d"(argument2) : "memory");
	return result;
}

static int32_t
nxu_syscall4(uint32_t number, uint32_t argument0, uint32_t argument1, uint32_t argument2, uint32_t argument3)
{
	int32_t result;
	__asm__ volatile("int $0x80" : "=a"(result) : "a"(number), "b"(argument0), "c"(argument1), "d"(argument2), "S"(argument3) : "memory");
	return result;
}

static int32_t
nxu_syscall5(uint32_t number, uint32_t argument0, uint32_t argument1, uint32_t argument2, uint32_t argument3, uint32_t argument4)
{
	int32_t result;
	__asm__ volatile("int $0x80" : "=a"(result) : "a"(number), "b"(argument0), "c"(argument1), "d"(argument2), "S"(argument3), "D"(argument4) : "memory");
	return result;
}

/*
 * Six arguments use every register, ebp included, and ebp is the frame
 * pointer the compiler may be relying on, so it cannot be an asm operand.
 * The arguments go through a small array instead; the asm loads them, saves
 * and restores ebp and ebx itself, and the compiler only sees the array
 * pointer (in esi) and the result (in eax).
 */
static int32_t
nxu_syscall6(uint32_t number, uint32_t argument0, uint32_t argument1, uint32_t argument2, uint32_t argument3, uint32_t argument4, uint32_t argument5)
{
	uint32_t arguments[6] = { argument0, argument1, argument2, argument3, argument4, argument5 };
	const uint32_t *pointer = arguments;
	int32_t result;

	__asm__ volatile(
		"pushl %%ebp\n\t"
		"pushl %%ebx\n\t"
		"movl 0(%%esi), %%ebx\n\t"
		"movl 4(%%esi), %%ecx\n\t"
		"movl 8(%%esi), %%edx\n\t"
		"movl 16(%%esi), %%edi\n\t"
		"movl 20(%%esi), %%ebp\n\t"
		"movl 12(%%esi), %%esi\n\t"
		"int $0x80\n\t"
		"popl %%ebx\n\t"
		"popl %%ebp"
		: "=a"(result), "+S"(pointer)
		: "a"(number)
		: "ecx", "edx", "edi", "memory", "cc"
	);
	return result;
}

/* Reject a 64-bit argument that cannot be represented in 32 bits. */
#define NXU_REQUIRE_U32(value) \
	do { \
		if ((uint64_t)(value) > 0xFFFFFFFFULL) return -NXU_SYS_E_INVALID_ARGUMENT; \
	} while (0)

/* Same, for a length whose successful result is itself returned in eax. */
#define NXU_REQUIRE_LENGTH(value) \
	do { \
		if ((uint64_t)(value) > 0x7FFFFFFFULL) return -NXU_SYS_E_INVALID_ARGUMENT; \
	} while (0)

#define NXU_PTR(pointer) ((uint32_t)(uintptr_t)(pointer))

/* A result that is either a user address (zero-extended) or a small negative error. */
static int64_t
nxu_address_result(int32_t raw)
{
	if ((uint32_t)raw >= 0U - NXU_ERROR_RANGE) return (int64_t)raw;
	return (int64_t)(uint32_t)raw;
}

int64_t
nxu_exit(uint64_t status)
{
	return nxu_syscall1(NXU_SYS_EXIT, (uint32_t)status);
}

int64_t
nxu_write(uint64_t descriptor, const void *buffer, uint64_t length)
{
	NXU_REQUIRE_U32(descriptor);
	NXU_REQUIRE_LENGTH(length);
	return nxu_syscall3(NXU_SYS_WRITE, (uint32_t)descriptor, NXU_PTR(buffer), (uint32_t)length);
}

int64_t
nxu_get_version(void *buffer, uint64_t capacity)
{
	NXU_REQUIRE_LENGTH(capacity);
	return nxu_syscall2(NXU_SYS_GET_VERSION, NXU_PTR(buffer), (uint32_t)capacity);
}

int64_t
nxu_open(const char *path, uint64_t flags)
{
	NXU_REQUIRE_U32(flags);
	return nxu_syscall2(NXU_SYS_OPEN, NXU_PTR(path), (uint32_t)flags);
}

int64_t
nxu_read(uint64_t descriptor, void *buffer, uint64_t length)
{
	NXU_REQUIRE_U32(descriptor);
	NXU_REQUIRE_LENGTH(length);
	return nxu_syscall3(NXU_SYS_READ, (uint32_t)descriptor, NXU_PTR(buffer), (uint32_t)length);
}

int64_t
nxu_close(uint64_t descriptor)
{
	NXU_REQUIRE_U32(descriptor);
	return nxu_syscall1(NXU_SYS_CLOSE, (uint32_t)descriptor);
}

int64_t
nxu_spawn(const char *path, const char *name)
{
	return nxu_syscall3(NXU_SYS_SPAWN, NXU_PTR(path), NXU_PTR(name), 0U);
}

int64_t
nxu_spawn_caps(const char *path, const char *name, uint64_t caps)
{
	return nxu_syscall3(NXU_SYS_SPAWN, NXU_PTR(path), NXU_PTR(name), (uint32_t)caps);
}

int64_t
nxu_get_caps(void)
{
	return nxu_syscall0(NXU_SYS_GET_CAPS);
}

int64_t
nxu_ioctl(uint64_t descriptor, uint64_t command, void *argument)
{
	NXU_REQUIRE_U32(descriptor);
	NXU_REQUIRE_U32(command);
	return nxu_syscall3(NXU_SYS_IOCTL, (uint32_t)descriptor, (uint32_t)command, NXU_PTR(argument));
}

int64_t
nxu_waitpid(uint64_t pid, uint64_t *status)
{
	NXU_REQUIRE_U32(pid);
	return nxu_syscall2(NXU_SYS_WAITPID, (uint32_t)pid, NXU_PTR(status));
}

int64_t
nxu_getpid(void)
{
	return nxu_syscall0(NXU_SYS_GETPID);
}

int64_t
nxu_yield(void)
{
	return nxu_syscall0(NXU_SYS_YIELD);
}

int64_t
nxu_get_boot_args(char *buffer, uint64_t capacity)
{
	NXU_REQUIRE_LENGTH(capacity);
	return nxu_syscall2(NXU_SYS_GET_BOOT_ARGS, NXU_PTR(buffer), (uint32_t)capacity);
}

int64_t
nxu_klog_read(uint64_t *cursor, char *buffer, uint64_t capacity)
{
	NXU_REQUIRE_LENGTH(capacity);
	return nxu_syscall3(NXU_SYS_KLOG_READ, NXU_PTR(cursor), NXU_PTR(buffer), (uint32_t)capacity);
}

int64_t
nxu_unlink(const char *path)
{
	return nxu_syscall1(NXU_SYS_UNLINK, NXU_PTR(path));
}

int64_t
nxu_sync(void)
{
	return nxu_syscall0(NXU_SYS_SYNC);
}

/* Last widened uptime, so the next call can tell a 32-bit wrap from a stale sample. */
static uint64_t g_uptime_last;

int64_t
nxu_uptime_us(void)
{
	uint32_t low = (uint32_t)nxu_syscall0(NXU_SYS_UPTIME_US);
	uint64_t last = __atomic_load_n(&g_uptime_last, __ATOMIC_RELAXED);
	uint32_t last_low = (uint32_t)last;
	uint64_t value = (last & ~0xFFFFFFFFULL) | low;

	/*
	 * A backwards jump of more than half the range is the counter wrapping
	 * forward past the last sample; a forward jump of more than half the
	 * range is a sample read just before another thread published a value
	 * past a wrap.
	 */
	if (low < last_low && last_low - low > 0x80000000U) value += 0x100000000ULL;
	else if (low > last_low && low - last_low > 0x80000000U && value >= 0x100000000ULL) value -= 0x100000000ULL;

	if (value > last) __atomic_store_n(&g_uptime_last, value, __ATOMIC_RELAXED);
	return (int64_t)value;
}

int64_t
nxu_readdir(uint64_t descriptor, nxu_dirent_t *entry)
{
	NXU_REQUIRE_U32(descriptor);
	return nxu_syscall2(NXU_SYS_READDIR, (uint32_t)descriptor, NXU_PTR(entry));
}

/* A 32-bit process cannot address a file offset beyond 4 GiB - 1; such a seek is refused. */
int64_t
nxu_seek(uint64_t descriptor, uint64_t offset)
{
	NXU_REQUIRE_U32(descriptor);
	NXU_REQUIRE_U32(offset);
	return nxu_syscall2(NXU_SYS_SEEK, (uint32_t)descriptor, (uint32_t)offset);
}

int64_t
nxu_stat(const char *path, nxu_stat_t *stat)
{
	return nxu_syscall2(NXU_SYS_STAT, NXU_PTR(path), NXU_PTR(stat));
}

int64_t
nxu_mkdir(const char *path)
{
	return nxu_syscall1(NXU_SYS_MKDIR, NXU_PTR(path));
}

int64_t
nxu_recovery_fs_mount_info(uint32_t index, nxu_recovery_fs_mount_info_t *info)
{
	return nxu_syscall2(NXU_SYS_RECOVERY_FS_MOUNT_INFO, index, NXU_PTR(info));
}

int64_t
nxu_recovery_fs_mount(const char *filesystem, uint32_t device_index, const char *path)
{
	return nxu_syscall3(NXU_SYS_RECOVERY_FS_MOUNT, NXU_PTR(filesystem), device_index, NXU_PTR(path));
}

int64_t
nxu_recovery_fs_unmount(const char *path)
{
	return nxu_syscall1(NXU_SYS_RECOVERY_FS_UNMOUNT, NXU_PTR(path));
}

int64_t
nxu_recovery_block_info(uint32_t index, nxu_recovery_block_info_t *info)
{
	return nxu_syscall2(NXU_SYS_RECOVERY_BLOCK_INFO, index, NXU_PTR(info));
}

int64_t
nxu_recovery_fs_space_info(const char *path, nxu_recovery_fs_space_info_t *info)
{
	return nxu_syscall2(NXU_SYS_RECOVERY_FS_SPACE_INFO, NXU_PTR(path), NXU_PTR(info));
}

int64_t
nxu_recovery_block_layout_info(uint32_t index, nxu_recovery_block_layout_info_t *info)
{
	return nxu_syscall2(NXU_SYS_RECOVERY_BLOCK_LAYOUT_INFO, index, NXU_PTR(info));
}

int64_t
nxu_recovery_block_partition_info(uint32_t device_index, uint32_t partition_index, nxu_recovery_block_partition_info_t *info)
{
	return nxu_syscall3(NXU_SYS_RECOVERY_BLOCK_PARTITION_INFO, device_index, partition_index, NXU_PTR(info));
}

int64_t
nxu_recovery_block_health_info(uint32_t device_index, nxu_recovery_block_health_info_t *info)
{
	return nxu_syscall2(NXU_SYS_RECOVERY_BLOCK_HEALTH_INFO, device_index, NXU_PTR(info));
}

int64_t
nxu_recovery_block_verify(uint32_t device_index)
{
	return nxu_syscall1(NXU_SYS_RECOVERY_BLOCK_VERIFY, device_index);
}

int64_t
nxu_recovery_display_info(nxu_recovery_display_info_t *info)
{
	return nxu_syscall1(NXU_SYS_RECOVERY_DISPLAY_INFO, NXU_PTR(info));
}

int64_t
nxu_recovery_present(const uint32_t *pixels, uint32_t stride, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	return nxu_syscall6(NXU_SYS_RECOVERY_PRESENT, NXU_PTR(pixels), stride, x, y, width, height);
}

int64_t
nxu_recovery_input(nxu_recovery_input_event_t *event)
{
	return nxu_syscall1(NXU_SYS_RECOVERY_INPUT, NXU_PTR(event));
}

int64_t
nxu_system_reset(void)
{
	return nxu_syscall0(NXU_SYS_SYSTEM_RESET);
}

int64_t
nxu_ipc_port_allocate(void)
{
	return nxu_syscall0(NXU_SYS_IPC_PORT_ALLOCATE);
}

int64_t
nxu_ipc_port_deallocate(uint32_t name)
{
	return nxu_syscall1(NXU_SYS_IPC_PORT_DEALLOCATE, name);
}

int64_t
nxu_ipc_bootstrap_port(void)
{
	return nxu_syscall0(NXU_SYS_IPC_BOOTSTRAP_PORT);
}

int64_t
nxu_ipc_send(uint32_t dest_name, const void *buffer, uint64_t length, uint32_t xfer_name)
{
	NXU_REQUIRE_LENGTH(length);
	return nxu_syscall4(NXU_SYS_IPC_SEND, dest_name, NXU_PTR(buffer), (uint32_t)length, xfer_name);
}

int64_t
nxu_ipc_receive(uint32_t port_name, void *buffer, uint64_t capacity, uint32_t *out_xfer_name, uint32_t *out_xfer_type)
{
	NXU_REQUIRE_LENGTH(capacity);
	return nxu_syscall5(NXU_SYS_IPC_RECEIVE, port_name, NXU_PTR(buffer), (uint32_t)capacity, NXU_PTR(out_xfer_name), NXU_PTR(out_xfer_type));
}

int64_t
nxu_ipc_receive_wait(uint32_t port_name, void *buffer, uint64_t capacity, uint32_t *out_xfer_name, uint32_t *out_xfer_type)
{
	NXU_REQUIRE_LENGTH(capacity);
	return nxu_syscall5(NXU_SYS_IPC_RECEIVE_WAIT, port_name, NXU_PTR(buffer), (uint32_t)capacity, NXU_PTR(out_xfer_name), NXU_PTR(out_xfer_type));
}

int64_t
nxu_ipc_register_bootstrap(uint32_t port_name)
{
	return nxu_syscall1(NXU_SYS_IPC_REGISTER_BOOTSTRAP, port_name);
}

int64_t
nxu_display_claim(void)
{
	return nxu_syscall0(NXU_SYS_DISPLAY_CLAIM);
}

int64_t
nxu_shm_create(uint64_t size)
{
	NXU_REQUIRE_LENGTH(size);
	return nxu_syscall1(NXU_SYS_SHM_CREATE, (uint32_t)size);
}

int64_t
nxu_shm_map(uint64_t id)
{
	NXU_REQUIRE_U32(id);
	return nxu_address_result(nxu_syscall1(NXU_SYS_SHM_MAP, (uint32_t)id));
}

int64_t
nxu_mmap(uint64_t size, uint64_t prot_flags)
{
	NXU_REQUIRE_LENGTH(size);
	NXU_REQUIRE_U32(prot_flags);
	return nxu_address_result(nxu_syscall2(NXU_SYS_MMAP, (uint32_t)size, (uint32_t)prot_flags));
}

int64_t
nxu_munmap(uint64_t address, uint64_t size)
{
	NXU_REQUIRE_U32(address);
	NXU_REQUIRE_LENGTH(size);
	return nxu_syscall2(NXU_SYS_MUNMAP, (uint32_t)address, (uint32_t)size);
}

int64_t
nxu_thread_create(void (*entry)(void *arg), void *stack, void *arg)
{
	return nxu_syscall3(NXU_SYS_THREAD_CREATE, NXU_PTR(entry), NXU_PTR(stack), NXU_PTR(arg));
}

int64_t
nxu_thread_exit(uint64_t status)
{
	return nxu_syscall1(NXU_SYS_THREAD_EXIT, (uint32_t)status);
}

int64_t
nxu_thread_self(void)
{
	return nxu_syscall0(NXU_SYS_THREAD_SELF);
}

int64_t
nxu_socket_listen(const char *name, uint32_t backlog)
{
	return nxu_syscall2(NXU_SYS_SOCKET_LISTEN, NXU_PTR(name), backlog);
}

int64_t
nxu_socket_connect(const char *name)
{
	return nxu_syscall1(NXU_SYS_SOCKET_CONNECT, NXU_PTR(name));
}

int64_t
nxu_socket_accept(uint64_t listen_descriptor)
{
	NXU_REQUIRE_U32(listen_descriptor);
	return nxu_syscall1(NXU_SYS_SOCKET_ACCEPT, (uint32_t)listen_descriptor);
}

int64_t
nxu_sleep_us(uint64_t microseconds)
{
	if (microseconds > 0xFFFFFFFFULL) microseconds = 0xFFFFFFFFULL;
	return nxu_syscall1(NXU_SYS_SLEEP_US, (uint32_t)microseconds);
}

int64_t
nxu_ui_connect(const nxu_ui_connect_t *info)
{
	return nxu_syscall1(NXU_SYS_UI_CONNECT, NXU_PTR(info));
}

int64_t
nxu_ui_receive(uint32_t connection, nxu_ui_message_t *message, uint32_t wait)
{
	return nxu_syscall3(NXU_SYS_UI_RECEIVE, connection, NXU_PTR(message), wait);
}

int64_t
nxu_ui_submit(uint32_t connection, const nxu_ui_submit_t *submit)
{
	return nxu_syscall2(NXU_SYS_UI_SUBMIT, connection, NXU_PTR(submit));
}

int64_t
nxu_ui_control(uint32_t operation, uint64_t argument)
{
	NXU_REQUIRE_U32(argument);
	return nxu_syscall2(NXU_SYS_UI_CONTROL, operation, (uint32_t)argument);
}
