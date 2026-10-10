#ifndef NXU_DRIVERS_VIDEO_UI_SERVICE_ACTIVITY_H
#define NXU_DRIVERS_VIDEO_UI_SERVICE_ACTIVITY_H

/*
 * What Activity Monitor shows, for UIServiceHostV5.get_activity: the CPUs'
 * scheduler load, physical memory, and one entry per live process with the
 * CPU time its threads have run (kern/sched_prism/sched.c counts it in
 * sched_tick). Shared by the arm64 and i386 hosts.
 */

#if defined(NXU_UI_SERVICE)
#include <UIService.h>
#else
#include <frameworks/include/nxu/activity.h>

typedef nxu_ui_process_info_t UIServiceProcessInfo;
typedef nxu_ui_cpu_info_t UIServiceCpuInfo;
typedef nxu_ui_activity_t UIServiceActivity;

#define UI_SERVICE_ACTIVITY_NAME_MAX NXU_ACTIVITY_NAME_MAX
#define UI_SERVICE_ACTIVITY_CPU_MAX NXU_ACTIVITY_CPU_MAX
#define UI_SERVICE_PROCESS_STATE_RUNNING NXU_ACTIVITY_PROCESS_STATE_RUNNING
#define UI_SERVICE_PROCESS_STATE_RUNNABLE NXU_ACTIVITY_PROCESS_STATE_RUNNABLE
#define UI_SERVICE_PROCESS_STATE_SLEEPING NXU_ACTIVITY_PROCESS_STATE_SLEEPING
#define UI_SERVICE_PROCESS_STATE_STOPPED NXU_ACTIVITY_PROCESS_STATE_STOPPED
#define UI_SERVICE_PROCESS_STATE_OTHER NXU_ACTIVITY_PROCESS_STATE_OTHER
#define UI_SERVICE_PROCESS_FLAG_KERNEL NXU_ACTIVITY_PROCESS_FLAG_KERNEL
#define UI_SERVICE_PROCESS_FLAG_SYSTEM NXU_ACTIVITY_PROCESS_FLAG_SYSTEM
#define UI_SERVICE_STATUS_OK 0U
#define UI_SERVICE_STATUS_INVALID_ARGUMENT 1U
#endif

#include <stdint.h>

uint32_t ui_service_get_activity(void *context, UIServiceActivity *activity, UIServiceProcessInfo *processes,
								 uint32_t capacity, uint32_t *count_out);

#endif
