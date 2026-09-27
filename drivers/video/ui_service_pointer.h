#ifndef NXU_DRIVERS_VIDEO_UI_SERVICE_POINTER_H
#define NXU_DRIVERS_VIDEO_UI_SERVICE_POINTER_H

#include <stdint.h>

/*
 * Mouse motion to pointer motion, for the desktop hosts
 * (platform/<arch>/services/ui_service.c).
 *
 * The mouse reports motion in the host's points (QEMU forwards the host's
 * own, already accelerated, deltas), while the pointer lives on a screen of
 * physical pixels, `scale_permille` / 1000 of them per point. Added as they
 * came, a 2x screen moved the pointer half as far as the host's cursor
 * would have gone. Scaled here, it goes as far: no acceleration of our own
 * on top, which would compound the host's.
 *
 * Fractions carry over to the next report, so at a scale that is not a
 * whole number (1.5x) slow movements are not rounded away.
 */
typedef struct {
	int32_t carry_x;
	int32_t carry_y;
} ui_pointer_motion_t;

static inline int32_t ui_pointer_scale_axis(int32_t delta, uint32_t scale_permille, int32_t *carry)
{
	int64_t total = (int64_t)delta * (int64_t)scale_permille + *carry;
	int64_t pixels = total / 1000;

	*carry = (int32_t)(total - pixels * 1000);
	return (int32_t)pixels;
}

static inline void ui_pointer_scale(ui_pointer_motion_t *motion, uint32_t scale_permille, int32_t *dx, int32_t *dy)
{
	if (scale_permille == 0U) scale_permille = 1000U;
	*dx = ui_pointer_scale_axis(*dx, scale_permille, &motion->carry_x);
	*dy = ui_pointer_scale_axis(*dy, scale_permille, &motion->carry_y);
}

#endif
