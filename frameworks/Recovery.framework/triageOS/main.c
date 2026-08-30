#include "ui.h"

#include <Recovery/RecoveryServices.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

#define TRIAGE_MAX_WIDTH 1920U
#define TRIAGE_MAX_HEIGHT 1080U

static uint32_t g_pixels[TRIAGE_MAX_WIDTH * TRIAGE_MAX_HEIGHT];

#define TRIAGE_CURSOR_WIDTH 16U
#define TRIAGE_CURSOR_HEIGHT 16U

static const uint16_t g_cursor_outer[TRIAGE_CURSOR_HEIGHT] = {
	0x8000U, 0xC000U, 0xE000U, 0xF000U, 0xF800U, 0xFC00U, 0xFE00U, 0xFF00U,
	0xFF80U, 0xFFC0U, 0xF800U, 0xCC00U, 0x8600U, 0x0600U, 0x0300U, 0x0300U
};

static const uint16_t g_cursor_inner[TRIAGE_CURSOR_HEIGHT] = {
	0x0000U, 0x4000U, 0x6000U, 0x7000U, 0x7800U, 0x7C00U, 0x7E00U, 0x7F00U,
	0x7F80U, 0x7C00U, 0x7000U, 0x4400U, 0x0200U, 0x0200U, 0x0100U, 0x0100U
};

static uint32_t g_cursor_backing[TRIAGE_CURSOR_WIDTH * TRIAGE_CURSOR_HEIGHT];
static bool g_cursor_visible;
static int32_t g_cursor_x;
static int32_t g_cursor_y;

static void cursor_restore(uint32_t stride, uint32_t width, uint32_t height)
{
	if (!g_cursor_visible) return;

	for (uint32_t row = 0U; row < TRIAGE_CURSOR_HEIGHT; row++) {
		int32_t py = g_cursor_y + (int32_t)row;
		if (py < 0 || (uint32_t)py >= height) continue;

		for (uint32_t column = 0U; column < TRIAGE_CURSOR_WIDTH; column++) {
			int32_t px = g_cursor_x + (int32_t)column;
			if (px < 0 || (uint32_t)px >= width) continue;
			g_pixels[(uint64_t)(uint32_t)py * stride + (uint32_t)px] =
				g_cursor_backing[row * TRIAGE_CURSOR_WIDTH + column];
		}
	}

	g_cursor_visible = false;
}

static void cursor_draw(uint32_t stride, uint32_t width, uint32_t height, int32_t x, int32_t y)
{
	g_cursor_x = x;
	g_cursor_y = y;

	for (uint32_t row = 0U; row < TRIAGE_CURSOR_HEIGHT; row++) {
		int32_t py = y + (int32_t)row;
		if (py < 0 || (uint32_t)py >= height) continue;

		for (uint32_t column = 0U; column < TRIAGE_CURSOR_WIDTH; column++) {
			int32_t px = x + (int32_t)column;
			if (px < 0 || (uint32_t)px >= width) continue;
			uint64_t pixel = (uint64_t)(uint32_t)py * stride + (uint32_t)px;
			g_cursor_backing[row * TRIAGE_CURSOR_WIDTH + column] = g_pixels[pixel];
			uint16_t bit = (uint16_t)(0x8000U >> column);
			if ((g_cursor_outer[row] & bit) == 0U) continue;
			g_pixels[pixel] = (g_cursor_inner[row] & bit) != 0U ? 0xFFFFFFFFU : 0xFF000000U;
		}
	}

	g_cursor_visible = true;
}

static triage_damage_t cursor_damage(int32_t old_x, int32_t old_y, int32_t new_x, int32_t new_y, uint32_t width, uint32_t height)
{
	int32_t left = old_x < new_x ? old_x : new_x;
	int32_t top = old_y < new_y ? old_y : new_y;
	int64_t old_right = (int64_t)old_x + TRIAGE_CURSOR_WIDTH;
	int64_t new_right = (int64_t)new_x + TRIAGE_CURSOR_WIDTH;
	int64_t old_bottom = (int64_t)old_y + TRIAGE_CURSOR_HEIGHT;
	int64_t new_bottom = (int64_t)new_y + TRIAGE_CURSOR_HEIGHT;
	int64_t right = old_right > new_right ? old_right : new_right;
	int64_t bottom = old_bottom > new_bottom ? old_bottom : new_bottom;

	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > (int64_t)width) right = width;
	if (bottom > (int64_t)height) bottom = height;
	if (right <= left || bottom <= top) return (triage_damage_t) { 0 };

	return (triage_damage_t) {
		.x = left,
		.y = top,
		.width = (uint32_t)(right - left),
		.height = (uint32_t)(bottom - top),
		.full = false,
		.valid = true
	};
}

static int32_t clamp_pointer(int64_t value, uint32_t extent)
{
	if (extent == 0U || value < 0) return 0;
	if ((uint64_t)value >= extent) return (int32_t)(extent - 1U);
	return (int32_t)value;
}

static bool present_damage(const triage_damage_t *damage, uint32_t stride, uint32_t width, uint32_t height)
{
	if (damage == 0 || !damage->valid) return true;
	if (damage->full) return nxu_recovery_present(g_pixels, stride, 0U, 0U, width, height) >= 0;
	return nxu_recovery_present(g_pixels, stride, (uint32_t)damage->x, (uint32_t)damage->y, damage->width, damage->height) >= 0;
}

__attribute__((noreturn))
int main(void)
{
	nxu_recovery_display_info_t display;
	if (nxu_recovery_display_info(&display) < 0 || display.width == 0U || display.height == 0U) {
		(void)recovery_write_string("triageOS: recovery display unavailable\n");
		for (;;) recovery_sched_wait();
	}

	if (display.width > TRIAGE_MAX_WIDTH || display.height > TRIAGE_MAX_HEIGHT) {
		(void)recovery_write_string("triageOS: display exceeds recovery surface limit\n");
		for (;;) recovery_sched_wait();
	}

	triage_ui_t ui;
	if (!triage_ui_init(&ui, g_pixels, display.width, display.height, TRIAGE_MAX_WIDTH)) {
		(void)recovery_write_string("triageOS: StartupOptionsUI initialization failed\n");
		for (;;) recovery_sched_wait();
	}

	int32_t pointer_x = (int32_t)(display.width / 2U);
	int32_t pointer_y = (int32_t)(display.height / 2U);
	triage_ui_render(&ui);
	cursor_draw(TRIAGE_MAX_WIDTH, display.width, display.height, pointer_x, pointer_y);
	triage_damage_t damage;
	if (!triage_ui_take_damage(&ui, &damage) || !present_damage(&damage, TRIAGE_MAX_WIDTH, display.width, display.height)) {
		(void)recovery_write_string("triageOS: initial present failed\n");
		for (;;) recovery_sched_wait();
	}

	(void)recovery_write_string("triageOS: StartupOptionsUI is live\n");
	int32_t raw_x = 0;
	int32_t raw_y = 0;
	uint32_t buttons = 0U;
	bool pointer_baseline = false;

	for (;;) {
		bool redraw = false;
		bool pointer_moved = false;
		int32_t old_pointer_x = pointer_x;
		int32_t old_pointer_y = pointer_y;
		nxu_recovery_input_event_t event;
		int64_t received;

		while ((received = nxu_recovery_input(&event)) > 0) {
			if (event.kind == NXU_RECOVERY_INPUT_KEY) {
				if (triage_ui_key(&ui, event.code, event.value, event.modifiers)) redraw = true;
				continue;
			}

			if (event.kind != NXU_RECOVERY_INPUT_POINTER) continue;

			/*
			 * The mouse may already have accumulated motion while Shift+R was
			 * sampled by the splash. Adopt the first committed packet as the
			 * recovery baseline instead of jumping the cursor by that history.
			 */
			if (!pointer_baseline) {
				raw_x = event.x;
				raw_y = event.y;
				pointer_baseline = true;
			}

			int64_t next_x = (int64_t)pointer_x + ((int64_t)event.x - raw_x);
			int64_t next_y = (int64_t)pointer_y + ((int64_t)event.y - raw_y);
			raw_x = event.x;
			raw_y = event.y;
			int32_t new_x = clamp_pointer(next_x, display.width);
			int32_t new_y = clamp_pointer(next_y, display.height);
			if (new_x != pointer_x || new_y != pointer_y) {
				if (!pointer_moved) {
					cursor_restore(TRIAGE_MAX_WIDTH, display.width, display.height);
					pointer_moved = true;
				}

				if (triage_ui_pointer_move(&ui, new_x, new_y)) redraw = true;
			}

			pointer_x = new_x;
			pointer_y = new_y;

			uint32_t changed = buttons ^ event.buttons;
			if ((changed & event.buttons) != 0U && triage_ui_pointer_down(&ui, pointer_x, pointer_y)) redraw = true;
			if ((changed & buttons) != 0U && triage_ui_pointer_up(&ui, pointer_x, pointer_y)) redraw = true;
			buttons = event.buttons;
		}

		triage_action_t action = triage_ui_take_action(&ui);
		if (action == TRIAGE_ACTION_REINSTALL) {
			triage_ui_show_message(&ui, "Reinstall sevOS", "System image installation is not implemented in NXU recovery yet.");
			redraw = true;
		}

		if (action == TRIAGE_ACTION_RESTART) {
			if (recovery_system_reset() < 0) {
				triage_ui_show_message(&ui, "Restart failed", "The platform reset request was rejected.");
				redraw = true;
			}
		}

		if (redraw && !pointer_moved) cursor_restore(TRIAGE_MAX_WIDTH, display.width, display.height);

		if (redraw) triage_ui_render(&ui);

		if (redraw || pointer_moved) {
			cursor_draw(TRIAGE_MAX_WIDTH, display.width, display.height, pointer_x, pointer_y);

			if (redraw && triage_ui_take_damage(&ui, &damage) && !present_damage(&damage, TRIAGE_MAX_WIDTH, display.width, display.height)) {
				(void)recovery_write_string("triageOS: UI present failed\n");
			}

			if (pointer_moved) {
				triage_damage_t pointer_damage = cursor_damage(old_pointer_x, old_pointer_y, pointer_x, pointer_y, display.width, display.height);
				if (!present_damage(&pointer_damage, TRIAGE_MAX_WIDTH, display.width, display.height)) {
					(void)recovery_write_string("triageOS: cursor present failed\n");
				}
			}
		}

		recovery_sched_wait();
	}
}
