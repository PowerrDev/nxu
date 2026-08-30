#ifndef NXU_DRIVERS_VIDEO_UI_SERVICE_HOST_H
#define NXU_DRIVERS_VIDEO_UI_SERVICE_HOST_H

#include <stdbool.h>

/*
 * Enter the early interactive UIService runtime used while NXU is still bringing up
 * the complete userspace compositor path.
 */
bool ui_service_bootstrap(void);

#endif
