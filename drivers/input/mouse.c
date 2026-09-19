#include <kern/console/console.h>
#include <drivers/input/mouse.h>

#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EV_SYN 0U
#define EV_KEY 1U
#define EV_REL 2U
#define SYN_REPORT 0U
#define REL_X 0U
#define REL_Y 1U
#define REL_WHEEL 8U
#define BTN_LEFT 0x110U
#define BTN_RIGHT 0x111U
#define BTN_MIDDLE 0x112U

/*
 * Sized well above one virtio-input burst between polls: enough for a
 * deliberate shake (many small direction-reversing packets arriving faster
 * than the UI service loop drains them) to survive as individual samples
 * instead of collapsing into one net delta -- see mouse_push_delta().
 */
#define MOUSE_DELTA_QUEUE_LEN 128U

typedef struct {
	int32_t dx;
	int32_t dy;
} mouse_delta_t;

typedef struct {
	uint32_t device_id;
	uint32_t buttons;
	int64_t x;
	int64_t y;
	int64_t wheel;
	int32_t pending_x;
	int32_t pending_y;
	int32_t pending_wheel;
	uint64_t events;
	uint64_t packets;
	mouse_delta_t delta_queue[MOUSE_DELTA_QUEUE_LEN];
	uint32_t delta_head;
	uint32_t delta_tail;
	uint32_t delta_count;
	bool initialized;
	bool present;
} mouse_state_t;

static mouse_state_t g_mouse;

/*
 * mouse_init:
 *
 * Reset relative-pointer class state.
 */
bool mouse_init(void)
{
	if (g_mouse.initialized) return true;
	memset(&g_mouse, 0, sizeof(g_mouse));
	g_mouse.initialized = true;
	return true;
}

/*
 * mouse_attach:
 *
 * Select one normalized input device as the active system mouse.
 */
bool mouse_attach(uint32_t device_id)
{
	if (!g_mouse.initialized || g_mouse.present || device_id == 0U) return false;
	g_mouse.device_id = device_id;
	g_mouse.present = true;
	return true;
}

/*
 * mouse_button:
 *
 * Apply one pointer button transition to the pending button mask.
 */
static void mouse_button(uint16_t code, bool down)
{
	uint32_t mask;

	switch (code) {
	case BTN_LEFT: mask = MOUSE_BUTTON_LEFT; break;
	case BTN_RIGHT: mask = MOUSE_BUTTON_RIGHT; break;
	case BTN_MIDDLE: mask = MOUSE_BUTTON_MIDDLE; break;
	default: return;
	}

	if (down) {
		g_mouse.buttons |= mask;
	} else {
		g_mouse.buttons &= ~mask;
	}
}

/*
 * mouse_push_delta:
 *
 * Queue one committed packet's relative motion for mouse_take_delta() to pop
 * later, one packet at a time. This is what lets a caller replay a burst of
 * motion as discrete samples instead of only ever seeing where the cumulative
 * position ended up -- see mouse_take_delta()'s doc comment for why that
 * distinction matters.
 *
 * On overflow, the oldest queued sample is simply dropped, not folded into
 * the next-oldest one: folding would preserve net displacement, but a shake
 * is a run of *opposite-signed* adjacent samples, so folding two of them
 * together is close to the worst possible thing to do here -- it cancels
 * towards zero and erases exactly the oscillation a shake needs to be
 * detected from. A dropped sample costs a few pixels of position accuracy
 * (invisible on screen); folding costs the whole gesture.
 */
static void mouse_push_delta(int32_t dx, int32_t dy)
{
	if (g_mouse.delta_count == MOUSE_DELTA_QUEUE_LEN) {
		g_mouse.delta_head = (g_mouse.delta_head + 1U) % MOUSE_DELTA_QUEUE_LEN;
		g_mouse.delta_count--;
	}

	g_mouse.delta_queue[g_mouse.delta_tail].dx = dx;
	g_mouse.delta_queue[g_mouse.delta_tail].dy = dy;
	g_mouse.delta_tail = (g_mouse.delta_tail + 1U) % MOUSE_DELTA_QUEUE_LEN;
	g_mouse.delta_count++;
}

/*
 * mouse_handle_event:
 *
 * Accumulate relative axes and button state until SYN_REPORT commits one
 * coherent pointer packet.
 */
void mouse_handle_event(const input_event_t *event)
{
	if (
		event == 0 ||
		!g_mouse.present ||
		event->device_id != g_mouse.device_id
	) {
		return;
	}

	g_mouse.events++;

	if (event->type == EV_REL) {
		switch (event->code) {
		case REL_X: g_mouse.pending_x += event->value; break;
		case REL_Y: g_mouse.pending_y += event->value; break;
		case REL_WHEEL: g_mouse.pending_wheel += event->value; break;
		default: break;
		}
		return;
	}

	if (event->type == EV_KEY) {
		mouse_button(event->code, event->value != 0);
		return;
	}

	if (event->type == EV_SYN && event->code == SYN_REPORT) {
		g_mouse.x += g_mouse.pending_x;
		g_mouse.y += g_mouse.pending_y;
		g_mouse.wheel += g_mouse.pending_wheel;

		if (g_mouse.pending_x != 0 || g_mouse.pending_y != 0) {
			mouse_push_delta(g_mouse.pending_x, g_mouse.pending_y);
		}

		g_mouse.pending_x = 0;
		g_mouse.pending_y = 0;
		g_mouse.pending_wheel = 0;
		g_mouse.packets++;
	}
}

bool mouse_is_present(void)
{
	return g_mouse.present;
}

uint32_t mouse_device_id(void)
{
	return g_mouse.present ? g_mouse.device_id : 0U;
}

uint32_t mouse_buttons(void)
{
	return g_mouse.buttons;
}

int64_t mouse_x(void)
{
	return g_mouse.x;
}

int64_t mouse_y(void)
{
	return g_mouse.y;
}

int64_t mouse_wheel(void)
{
	return g_mouse.wheel;
}

/*
 * mouse_take_delta:
 *
 * Pop the oldest queued per-packet relative motion, one packet at a time.
 *
 * This exists alongside mouse_x()/mouse_y() rather than replacing them:
 * those report where the cumulative position ended up, which collapses a
 * burst of packets (several arriving before a caller gets around to polling)
 * into a single net delta. That is fine for "where is the pointer", but it
 * silently erases back-and-forth motion -- exactly what a caller trying to
 * detect a physical shake gesture from individual samples needs to see.
 *
 * Returns false (dx/dy untouched) once the queue is empty; call in a loop
 * until it does to drain everything currently queued.
 */
bool mouse_take_delta(int32_t *dx, int32_t *dy)
{
	if (dx == 0 || dy == 0 || g_mouse.delta_count == 0U) return false;

	*dx = g_mouse.delta_queue[g_mouse.delta_head].dx;
	*dy = g_mouse.delta_queue[g_mouse.delta_head].dy;
	g_mouse.delta_head = (g_mouse.delta_head + 1U) % MOUSE_DELTA_QUEUE_LEN;
	g_mouse.delta_count--;
	return true;
}

uint64_t mouse_event_count(void)
{
	return g_mouse.events;
}

uint64_t mouse_packet_count(void)
{
	return g_mouse.packets;
}

static void mouse_put_signed(int64_t value)
{
	if (value < 0) {
		kputc('-');
		kputu64(0ULL - (uint64_t)value);
	} else {
		kputu64((uint64_t)value);
	}
}

/*
 * mouse_dump_state:
 *
 * Print committed pointer state only after observable packet activity
 * changes.
 */
void mouse_dump_state(void)
{
	if (!g_mouse.present) return;

	kputs("mouse: events ");
	kputu64(g_mouse.events);
	kputs(", packets ");
	kputu64(g_mouse.packets);
	kputs(", x ");
	mouse_put_signed(g_mouse.x);
	kputs(", y ");
	mouse_put_signed(g_mouse.y);
	kputs(", wheel ");
	mouse_put_signed(g_mouse.wheel);
	kputs(", buttons 0x");
	kputhex_byte((uint8_t)g_mouse.buttons);
	kputc('\n');
}
