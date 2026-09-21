#ifndef NXU_DRIVERS_VIDEO_UI_SERVICE_HOST_H
#define NXU_DRIVERS_VIDEO_UI_SERVICE_HOST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Enter the early interactive UIService runtime used while NXU is still bringing up
 * the complete userspace compositor path.
 */
bool ui_service_bootstrap(void);

/*
 * The session runs its event loop on the thread that called
 * ui_service_bootstrap and never returns. When other threads and processes
 * must run at the same time (the unified boot), that thread has to hand the
 * CPU over: with this set, the loop yields to the scheduler on every event
 * poll and while it waits for the next frame. Off by default: a boot that
 * only runs the session never yields.
 */
void ui_service_set_cooperative(bool cooperative);

/* True from the moment the app's event loop starts until it returns (it should not). */
bool ui_service_running(void);

/* How many times the event loop has polled for input; it moves only while the loop is alive. */
uint64_t ui_service_poll_count(void);

#endif
