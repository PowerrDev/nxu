#ifndef NXU_CRASH_DEFS_H
#define NXU_CRASH_DEFS_H
#include <stdint.h>
#include <kern/syscall/dyrld_defs.h>
#define NXU_CRASH_VERSION 2U
#define NXU_CRASH_IMAGES (NXU_DYRLD_IMAGES + 1U)
#define NXU_CRASH_THREADS 32U
#define NXU_CRASH_FRAMES 32U
#define NXU_CRASH_PATH_MAX 256U
#define NXU_CRASH_ARM64 1U
#define NXU_CRASH_I386 2U
#define NXU_CRASH_REGISTERS_VALID 1U
#define NXU_CRASH_STACK_INCOMPLETE 2U
#define NXU_CRASH_THREADS_TRUNCATED 1U
#define NXU_CRASH_SNAPSHOT_INCOMPLETE 2U
/* NEXT sleeps until a report exists. ACK removes only the exact saved report.
 * A failed writer can NEXT again without losing its report. RELEASE relinquishes
 * the exclusive consumer lease (also automatic on process exit). */
#define NXU_CRASH_NEXT 1U
#define NXU_CRASH_ACK 2U
#define NXU_CRASH_RELEASE 3U
#define NXU_CRASH_NOTIFY 4U
#define NXU_CRASH_UI_NEXT 5U

typedef struct {
	uint64_t incident;
	char name[32], report[512], executable[NXU_CRASH_PATH_MAX];
} nxu_crash_notification_t;

typedef struct {
	uint64_t tid, pc, sp, fp, status, fault_address, syndrome;
	uint64_t registers[31];
	uint64_t frames[NXU_CRASH_FRAMES];
	uint32_t frame_count, flags;
} nxu_crash_thread_t;

typedef struct {
	uint32_t version, struct_size;
	uint64_t incident, uptime_us, uniqueid, faulting_tid, dropped;
	uint32_t pid, ppid, signal, architecture, thread_count, flags, capabilities, reserved;
	char name[32];
	char executable[NXU_CRASH_PATH_MAX];
	uint8_t image_id[32]; /* SHA-256 of PT_LOAD file bytes, in program-header order */
	nxu_crash_thread_t threads[NXU_CRASH_THREADS];
	uint32_t image_count, reserved_images;
	nxu_dyrld_image_t images[NXU_CRASH_IMAGES];
} nxu_crash_report_t;

_Static_assert(sizeof(nxu_crash_thread_t) == 568U, "crash thread ABI");
_Static_assert(sizeof(nxu_crash_report_t) == 30688U, "crash report ABI");
_Static_assert(sizeof(nxu_crash_notification_t) == 808U, "crash notification ABI");
#endif
