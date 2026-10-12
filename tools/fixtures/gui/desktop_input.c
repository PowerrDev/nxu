/* Exercise the production desktop callback with queued input and idle wakes. */

#define main desktop_service_main
#include <frameworks/BootDaemons.framework/desktop_service.c>

#undef main
#include <assert.h>
#include <stdio.h>

static nxu_recovery_input_event_t queued[8];
static uint32_t count;
static uint32_t cursor;
static uint32_t waits;
static uint32_t moves;

bool
CoreGraphicsConnectionAlive(void)
{
	return true;
}

int64_t
nxu_recovery_input(nxu_recovery_input_event_t *event)
{
	if (cursor == count) {
		return 0;
	}

	*event = queued[cursor++];
	return 1;
}

int64_t
nxu_event_wait(uint32_t timeout, uint32_t sequence)
{
	assert(!sequence || sequence == 7);
	if (timeout) {
		assert(timeout == 16667);
		waits++;
	}

	return 7;
}

bool
CG_Pointer_Move(int32_t x, int32_t y)
{
	assert(x >= 0 && y >= 0);
	moves++;
	return true;
}

int64_t
nxu_write(uint64_t fd, const void *buffer, uint64_t length)
{
	(void)fd;
	(void)buffer;
	return length;
}

int64_t
nxu_exit(uint64_t status)
{
	(void)status;
	assert(false);
	return 0;
}

int
main(void)
{
	desktop.surface.width = 640;
	desktop.surface.height = 480;
	desktop.scale = 1000;
	desktop.x = 320;
	desktop.y = 240;
	queued[count++] = (nxu_recovery_input_event_t){.kind = NXU_RECOVERY_INPUT_POINTER, .x = 20, .y = 20};
	queued[count++] = (nxu_recovery_input_event_t){.kind = NXU_RECOVERY_INPUT_POINTER, .x = 30, .y = 25, .buttons = 1};
	queued[count++] = (nxu_recovery_input_event_t){.kind = NXU_RECOVERY_INPUT_POINTER, .x = 30, .y = 25};
	queued[count++] = (nxu_recovery_input_event_t){.kind = NXU_RECOVERY_INPUT_SCROLL, .value = 3};
	queued[count++] = (nxu_recovery_input_event_t){.kind = NXU_RECOVERY_INPUT_SCROLL, .value = -2};
	CoreGraphicsHostEvent event;
	assert(poll_event(0, &event) == 0 && event.event_type == CORE_GRAPHICS_EVENT_POINTER_MOVED);
	assert(poll_event(0, &event) == 0 && event.event_type == CORE_GRAPHICS_EVENT_POINTER_DOWN && event.button == 1);
	assert(poll_event(0, &event) == 0 && event.event_type == CORE_GRAPHICS_EVENT_POINTER_UP && event.button == 1);
	assert(poll_event(0, &event) == 0 && event.event_type == CORE_GRAPHICS_EVENT_SCROLL &&
		   (int32_t)event.reserved == 3);
	assert(poll_event(0, &event) == 0 && event.event_type == CORE_GRAPHICS_EVENT_SCROLL &&
		   (int32_t)event.reserved == -2);
	assert(moves == 1 && !waits);
	assert(poll_event(0, &event) == 0 && !event.event_type && !waits);
	assert(poll_event(0, &event) == 0 && !event.event_type && waits == 1);
	puts("PASS queued motion/button order, signed scrolling, render-before-wait and idle blocking");
	return 0;
}
