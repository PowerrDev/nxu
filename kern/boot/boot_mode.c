/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/boot/boot_mode.c
 *
 * Detect early boot key combinations and select the requested startup mode.
 */
#include <kern/boot/boot_mode.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/splash.h>

#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

#define BOOT_MODE_EV_KEY 1U
#define BOOT_MODE_KEY_R 19U
#define BOOT_MODE_KEY_LEFT_SHIFT 42U
#define BOOT_MODE_KEY_RIGHT_SHIFT 54U
#define BOOT_MODE_KEY_RELEASE 0
#define BOOT_MODE_KEY_PRESS 1
#define BOOT_MODE_KEY_REPEAT 2

static boot_mode_t g_boot_mode;
static bool g_r_down;
static bool g_left_shift_down;
static bool g_right_shift_down;

void
boot_mode_init(void)
{
	g_boot_mode = boot_arg_bool("recovery", false) ? BOOT_MODE_TRIAGE_OS : BOOT_MODE_NORMAL;
	g_r_down = false;
	g_left_shift_down = false;
	g_right_shift_down = false;
}

void
boot_mode_force_recovery(void)
{
	g_boot_mode = BOOT_MODE_TRIAGE_OS;
}

static bool
boot_mode_key_down(int32_t value)
{
	return value == BOOT_MODE_KEY_PRESS || value == BOOT_MODE_KEY_REPEAT;
}

static bool
boot_mode_chord_down(void)
{
	return g_r_down && (g_left_shift_down || g_right_shift_down);
}

bool
boot_mode_poll(void)
{
	if (g_boot_mode == BOOT_MODE_TRIAGE_OS || !keyboard_is_present()) return false;

	input_event_t event;
	while (input_read(&event)) {
		if (event.device_class != INPUT_DEVICE_KEYBOARD || event.type != BOOT_MODE_EV_KEY) continue;

		if (event.value != BOOT_MODE_KEY_RELEASE && event.value != BOOT_MODE_KEY_PRESS &&
			event.value != BOOT_MODE_KEY_REPEAT)
			continue;

		bool down = boot_mode_key_down(event.value);

		switch (event.code) {
		case BOOT_MODE_KEY_R:
			g_r_down = down;
			break;
		case BOOT_MODE_KEY_LEFT_SHIFT:
			g_left_shift_down = down;
			break;
		case BOOT_MODE_KEY_RIGHT_SHIFT:
			g_right_shift_down = down;
			break;
		default:
			break;
		}

		if (!boot_mode_chord_down()) continue;

		g_boot_mode = BOOT_MODE_TRIAGE_OS;
		(void)boot_splash_set_status("Loading startup options...");
		kputln("boot: startup recovery options requested");
		return true;
	}

	return false;
}

boot_mode_t
boot_mode_current(void)
{
	return g_boot_mode;
}

bool
boot_mode_is_triage_os(void)
{
	return g_boot_mode == BOOT_MODE_TRIAGE_OS;
}

const char *
boot_mode_name(void)
{
	return g_boot_mode == BOOT_MODE_TRIAGE_OS ? "triageOS" : "sevOS";
}
