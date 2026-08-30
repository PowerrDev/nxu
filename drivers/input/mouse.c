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
