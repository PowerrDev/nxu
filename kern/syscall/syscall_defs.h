#ifndef NXU_KERN_SYSCALL_DEFS_H
#define NXU_KERN_SYSCALL_DEFS_H

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
#define NXU_SYS_IPC_PORT_ALLOCATE 34ULL
#define NXU_SYS_IPC_PORT_DEALLOCATE 35ULL
#define NXU_SYS_IPC_BOOTSTRAP_PORT 36ULL
#define NXU_SYS_IPC_SEND 37ULL
#define NXU_SYS_IPC_RECEIVE 38ULL
#define NXU_SYS_IPC_REGISTER_BOOTSTRAP 39ULL
#define NXU_SYS_DISPLAY_CLAIM 40ULL
#define NXU_SYS_SHM_CREATE 41ULL
#define NXU_SYS_SHM_MAP 42ULL
#define NXU_SYS_MMAP 43ULL
#define NXU_SYS_MUNMAP 44ULL
#define NXU_SYS_THREAD_CREATE 45ULL
#define NXU_SYS_THREAD_EXIT 46ULL
#define NXU_SYS_THREAD_SELF 47ULL
#define NXU_SYS_SOCKET_LISTEN 48ULL
#define NXU_SYS_SOCKET_CONNECT 49ULL
#define NXU_SYS_SOCKET_ACCEPT 50ULL
#define NXU_SYS_FORK 51ULL
#define NXU_SYS_EXEC 52ULL
#define NXU_SYS_KILL 53ULL
#define NXU_SYS_SIGACTION 54ULL
#define NXU_SYS_SIGPROCMASK 55ULL
#define NXU_SYS_SIGRETURN 56ULL
#define NXU_SYS_GETPPID 57ULL
#define NXU_SYS_WAIT 58ULL
#define NXU_SYS_IPC_RECEIVE_WAIT 59ULL
#define NXU_SYS_GET_CAPS 60ULL
#define NXU_SYS_IOCTL 61ULL
#define NXU_SYS_GETCPU 62ULL
#define NXU_SYS_SETAFFINITY 63ULL

/* wait(): pass as the pid to wait for any child. */
#define NXU_WAIT_ANY UINT64_MAX

/*
 * Capabilities: what a process may do to the system beyond computing and
 * talking over IPC. Default-deny -- a process has none unless it is PID 1
 * (which has all of them) or its parent granted a subset when it spawned it
 * (nxu_spawn_caps). fork and exec keep the caller's set. See p_caps in
 * kern/process/proc.h.
 *
 *   FS_WRITE   open for write/create/truncate/append, unlink, mkdir
 *   DISPLAY    claim the display (display_claim)
 *   RESET      reset the machine (system_reset)
 *   AUDIO      open the audio device (/dev/audio0)
 */
#define NXU_CAP_FS_WRITE (1U << 0U)
#define NXU_CAP_DISPLAY (1U << 1U)
#define NXU_CAP_RESET (1U << 2U)
#define NXU_CAP_AUDIO (1U << 3U)
#define NXU_CAP_ALL (NXU_CAP_FS_WRITE | NXU_CAP_DISPLAY | NXU_CAP_RESET | NXU_CAP_AUDIO)

/*
 * Exit status recorded for a process the kernel terminated (waitpid reports
 * it verbatim). A normal exit(n) leaves this bit clear. On arm64 the low
 * 8 bits are the NXU_SIG* number that killed the process; the i386 port
 * still stores the CPU trap vector there until it grows signals.
 */
#define NXU_EXIT_KILLED 0x80000000ULL
#define NXU_EXIT_KILLED_SIGNAL(signal) (NXU_EXIT_KILLED | ((uint64_t)(signal) & 0xFFULL))
#define NXU_EXIT_WAS_KILLED(status) (((status) & NXU_EXIT_KILLED) != 0ULL)
#define NXU_EXIT_TERMSIG(status) ((uint32_t)((status) & 0xFFULL))

/* Signal numbers (Linux numbering, so the values look familiar). */
#define NXU_SIGHUP 1U
#define NXU_SIGINT 2U
#define NXU_SIGQUIT 3U
#define NXU_SIGILL 4U
#define NXU_SIGTRAP 5U
#define NXU_SIGABRT 6U
#define NXU_SIGBUS 7U
#define NXU_SIGFPE 8U
#define NXU_SIGKILL 9U
#define NXU_SIGUSR1 10U
#define NXU_SIGSEGV 11U
#define NXU_SIGUSR2 12U
#define NXU_SIGPIPE 13U
#define NXU_SIGALRM 14U
#define NXU_SIGTERM 15U
#define NXU_SIGCHLD 17U
#define NXU_SIGCONT 18U
#define NXU_SIGSTOP 19U
#define NXU_NSIG 32U

/* sigaction handler values other than a user function address. */
#define NXU_SIG_DFL 0ULL
#define NXU_SIG_IGN 1ULL

/* sigaction flags. */
#define NXU_SA_NODEFER (1U << 0U) /* do not block the signal while its handler runs */

/* sigprocmask "how". */
#define NXU_SIG_BLOCK 0U
#define NXU_SIG_UNBLOCK 1U
#define NXU_SIG_SETMASK 2U

/* Signals that can be neither caught, blocked nor ignored. */
#define NXU_SIG_UNCATCHABLE_MASK (1U << NXU_SIGKILL)

/*
 * sigaction ABI. restorer is the address of a tiny user routine that ends in
 * the sigreturn syscall; the kernel points the handler's return address at it
 * (userland's libnxu supplies one). mask lists further signals to block while
 * the handler runs, on top of the signal itself.
 */
typedef struct {
	uint64_t handler;
	uint64_t restorer;
	uint32_t mask;
	uint32_t flags;
} nxu_sigaction_t;

/* exec accepts at most this many argv strings, this many bytes in all. */
#define NXU_EXEC_ARGV_MAX 16U
#define NXU_EXEC_ARGV_BYTES 1024U

/* Mirrors vm_user_protection_t (vm/address_space.h) numerically. */
#define NXU_MMAP_PROT_READ_WRITE 0U
#define NXU_MMAP_PROT_READ_ONLY 1U
#define NXU_MMAP_PROT_READ_EXECUTE 2U

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
#define NXU_SYS_E_DENIED 13LL
#define NXU_SYS_E_INTERRUPTED 14LL

/* Mirrors kern/ipc/ipc_types.h's ipc_kmsg_xfer_type_t numerically. */
#define NXU_IPC_XFER_NONE 0U
#define NXU_IPC_XFER_PORT 1U
#define NXU_IPC_XFER_MEMORY 2U

#define NXU_O_READ (1U << 0U)
#define NXU_O_WRITE (1U << 1U)
#define NXU_O_CREATE (1U << 2U)
#define NXU_O_TRUNCATE (1U << 3U)
#define NXU_O_APPEND (1U << 4U)

/*
 * Device files: a read or write that would have to wait fails with
 * -NXU_SYS_E_AGAIN (or is cut short) instead of sleeping.
 */
#define NXU_O_NONBLOCK (1U << 5U)

/*
 * The open flags that can change a file or create one: they need
 * NXU_CAP_FS_WRITE. Opening a device node for writing is not one of them; the
 * device asks for its own capability instead (NXU_CAP_AUDIO for /dev/audio0).
 */
#define NXU_O_MODIFYING (NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE | NXU_O_APPEND)

/*
 * ioctl request numbers: the direction of the argument, its size, a device
 * group and a number in the group, so the kernel can copy the argument in and
 * out without knowing the device. NXU_IOC_IN: the kernel reads it (a request
 * carrying a value); NXU_IOC_OUT: the kernel fills it in; both: negotiated
 * in place. Arguments are at most NXU_IOC_SIZE_MAX bytes.
 */
#define NXU_IOC_NONE 0U
#define NXU_IOC_IN 1U
#define NXU_IOC_OUT 2U
#define NXU_IOC_SIZE_MAX 256U

#define NXU_IOC(direction, group, number, size) \
	(((uint32_t)(direction) << 30U) | ((uint32_t)(size) << 16U) | ((uint32_t)(group) << 8U) | (uint32_t)(number))
#define NXU_IOC_DIRECTION(command) (((command) >> 30U) & 3U)
#define NXU_IOC_SIZE(command) (((command) >> 16U) & 0x3FFFU)
#define NXU_IOC_GROUP(command) (((command) >> 8U) & 0xFFU)
#define NXU_IOC_NUMBER(command) ((command) & 0xFFU)

#define NXU_IO(group, number) NXU_IOC(NXU_IOC_NONE, group, number, 0U)
#define NXU_IOR(group, number, type) NXU_IOC(NXU_IOC_OUT, group, number, sizeof(type))
#define NXU_IOW(group, number, type) NXU_IOC(NXU_IOC_IN, group, number, sizeof(type))
#define NXU_IOWR(group, number, type) NXU_IOC(NXU_IOC_IN | NXU_IOC_OUT, group, number, sizeof(type))

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
