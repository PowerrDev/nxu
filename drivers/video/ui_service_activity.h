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

#include <stdint.h>

uint32_t ui_service_get_activity(
	void *context,
	UIServiceActivity *activity,
	UIServiceProcessInfo *processes,
	uint32_t capacity,
	uint32_t *count_out
);

#endif

#endif
