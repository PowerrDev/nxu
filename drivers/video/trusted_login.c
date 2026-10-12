/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/trusted_login.c
 *
 * Render the kernel-owned authentication screen and handle secure input.
 */
#include <drivers/video/ui_service_host.h>
#include <drivers/video/display.h>
#include <drivers/video/login_wallpaper.h>
#include <reporter_fonts.h>
#include <drivers/video/login_font.h>
#include <drivers/video/ramfb_console.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/tep/tep_crypto.h>
#include <drivers/tep/tep_mailbox.h>
#include <platform/rtc.h>
#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <kern/process/thread.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <libk/string.h>

static void
wipe(void *memory, size_t size)
{
	volatile uint8_t *bytes = memory;

	while (size--)
		*bytes++ = 0;
}

#define LOGIN_TEXT 0xD8FFFFFFU
#define LOGIN_CLOCK_TEXT 0xB8FFFFFFU
#define LOGIN_FIELD_TEXT 0xFF4B5360U
#define LOGIN_FIELD_MUTED 0xFF737B88U

static uint32_t login_text_width(const reporter_font_t *font, const char *text, uint32_t scale);

static uint32_t
login_blend(uint32_t background, uint32_t foreground, uint32_t alpha)
{
	uint32_t color = 0U;

	for (uint32_t shift = 0U; shift < 24U; shift += 8U) {
		uint32_t back = (background >> shift) & 255U;
		uint32_t front = (foreground >> shift) & 255U;

		color |= ((back * (255U - alpha) + front * alpha + 127U) / 255U) << shift;
	}

	return color;
}

static void
login_pixel(display_device_t *display, int32_t x, int32_t y, uint32_t color)
{
	if (x >= 0 && y >= 0 && x < (int32_t)display->width && y < (int32_t)display->height) {
		display->framebuffer[(uint64_t)y * display->stride + x] = color;
	}
}

static void
login_blur_clock_mask(display_device_t *display, const uint32_t *wallpaper, uint32_t center, uint32_t y,
					  const char *text, const reporter_font_t *font)
{
	uint32_t width = login_text_width(font, text, 1U);
	uint32_t x = center > width / 2U ? center - width / 2U : 0U;

	while (*text) {
		uint32_t code = (uint8_t)*text++;
		const reporter_glyph_t *glyph = &font->glyphs[(code >= 32U && code <= 126U ? code : '?') - 32U];

		for (uint32_t row = 0U; row < glyph->height; row++) {
			for (uint32_t column = 0U; column < glyph->width; column++) {
				uint32_t coverage = font->pixels[glyph->offset + row * glyph->width + column];
				if (coverage == 0U) continue;

				int32_t px = (int32_t)x + glyph->x + (int32_t)column;
				int32_t py = (int32_t)y + glyph->y + (int32_t)row;
				if (px < 0 || py < 0 || px >= (int32_t)display->width || py >= (int32_t)display->height) continue;

				uint32_t sum[3] = {0U, 0U, 0U};
				uint32_t samples = 0U;
				for (int32_t oy = -4; oy <= 4; oy += 2) {
					for (int32_t ox = -4; ox <= 4; ox += 2) {
						int32_t sx = px + ox;
						int32_t sy = py + oy;
						if (sx < 0 || sy < 0 || sx >= (int32_t)display->width || sy >= (int32_t)display->height)
							continue;

						uint32_t pixel = wallpaper[(uint64_t)sy * display->width + (uint32_t)sx];
						for (uint32_t channel = 0U; channel < 3U; channel++) {
							sum[channel] += (pixel >> (channel * 8U)) & 255U;
						}
						samples++;
					}
				}

				uint32_t blurred = 0U;
				for (uint32_t channel = 0U; channel < 3U; channel++) {
					blurred |= (sum[channel] / samples) << (channel * 8U);
				}

				uint32_t *destination = &display->framebuffer[(uint64_t)py * display->stride + (uint32_t)px];
				*destination = login_blend(*destination, blurred, coverage * 160U / 255U);
			}
		}

		x += glyph->advance;
	}
}

static void
login_round_rect(display_device_t *display, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t radius,
				 uint32_t color)
{
	for (uint32_t row = 0U; row < height; row++) {
		for (uint32_t column = 0U; column < width; column++) {
			int32_t dx = 0;
			int32_t dy = 0;

			if (column < radius) {
				dx = (int32_t)(radius - column);
			} else if (column >= width - radius) {
				dx = (int32_t)(column - (width - radius - 1U));
			}

			if (row < radius) {
				dy = (int32_t)(radius - row);
			} else if (row >= height - radius) {
				dy = (int32_t)(row - (height - radius - 1U));
			}

			if ((uint32_t)(dx * dx + dy * dy) <= radius * radius) {
				if (color >> 24U) {
					uint32_t *pixel = &display->framebuffer[(uint64_t)(y + row) * display->stride + x + column];
					*pixel = login_blend(*pixel, color, color >> 24U);
				} else {
					login_pixel(display, x + column, y + row, color);
				}
			}
		}
	}
}

static uint32_t
login_text_width(const reporter_font_t *font, const char *text, uint32_t scale)
{
	uint32_t width = 0U;

	while (*text) {
		uint32_t code = (uint8_t)*text++;

		width += font->glyphs[(code >= 32U && code <= 126U ? code : '?') - 32U].advance * scale;
	}

	return width;
}

static void
login_label(display_device_t *display, uint32_t x, uint32_t y, const char *text, const reporter_font_t *font,
			uint32_t scale, uint32_t color)
{
	while (*text) {
		uint32_t code = (uint8_t)*text++;
		const reporter_glyph_t *glyph = &font->glyphs[(code >= 32U && code <= 126U ? code : '?') - 32U];

		for (uint32_t row = 0U; row < glyph->height; row++) {
			for (uint32_t column = 0U; column < glyph->width; column++) {
				uint32_t alpha = font->pixels[glyph->offset + row * glyph->width + column];

				for (uint32_t sy = 0U; sy < scale; sy++) {
					for (uint32_t sx = 0U; sx < scale; sx++) {
						int32_t px = (int32_t)x + (glyph->x + (int32_t)column) * (int32_t)scale + (int32_t)sx;
						int32_t py = (int32_t)y + (glyph->y + (int32_t)row) * (int32_t)scale + (int32_t)sy;

						if (px < 0 || py < 0 || px >= (int32_t)display->width || py >= (int32_t)display->height)
							continue;

						uint32_t *pixel = &display->framebuffer[(uint64_t)py * display->stride + px];
						uint32_t opacity = color >> 24U;

						*pixel = login_blend(*pixel, color, alpha * opacity / 255U);
					}
				}
			}
		}

		x += glyph->advance * scale;
	}
}

static void
login_center_label(display_device_t *display, uint32_t center, uint32_t y, const char *text,
				   const reporter_font_t *font, uint32_t scale, uint32_t color)
{
	uint32_t width = login_text_width(font, text, scale);

	login_label(display, center > width / 2U ? center - width / 2U : 0U, y, text, font, scale, color);
}

static void
login_format_clock(char date[40], char clock[8])
{
	static const char *weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
	static const char *months[] = {"January", "February", "March",	   "April",	  "May",	  "June",
								   "July",	  "August",	  "September", "October", "November", "December"};
	static const uint8_t month_days[] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
	uint64_t seconds = rtc_unix_time();
	uint64_t days = seconds / 86400U;
	uint32_t year = 1970U;

	while (days >= 365U + ((year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U)) {
		uint32_t year_days = 365U + ((year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U);
		days -= year_days;
		year++;
	}

	uint32_t month = 0U;
	while (month < 11U) {
		uint32_t length =
			month_days[month] + (month == 1U && year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U));

		if (days < length) break;

		days -= length;
		month++;
	}

	uint32_t day = (uint32_t)days + 1U;
	uint32_t weekday = (uint32_t)((seconds / 86400U + 4U) % 7U);
	uint32_t hour = (uint32_t)((seconds / 3600U) % 24U);
	uint32_t minute = (uint32_t)((seconds / 60U) % 60U);
	uint32_t display_hour = hour % 12U;
	if (display_hour == 0U) display_hour = 12U;

	char number[6];
	uint32_t cursor = 0U;
	const char *name = weekdays[weekday];
	while (*name) {
		date[cursor++] = *name++;
	}

	date[cursor++] = ',';
	date[cursor++] = ' ';
	name = months[month];
	while (*name) {
		date[cursor++] = *name++;
	}

	date[cursor++] = ' ';
	uint32_t digits = 0U;
	do {
		number[digits++] = (char)('0' + day % 10U);
		day /= 10U;
	} while (day != 0U);

	while (digits != 0U) {
		date[cursor++] = number[--digits];
	}

	date[cursor] = '\0';

	cursor = 0U;
	digits = 0U;
	do {
		number[digits++] = (char)('0' + display_hour % 10U);
		display_hour /= 10U;
	} while (display_hour != 0U);

	while (digits != 0U) {
		clock[cursor++] = number[--digits];
	}

	clock[cursor++] = ':';
	clock[cursor++] = (char)('0' + minute / 10U);
	clock[cursor++] = (char)('0' + minute % 10U);
	clock[cursor] = '\0';
}

static void
login_paint(display_device_t *display, const uint32_t *wallpaper, uint32_t length, bool setting, bool confirming,
			bool available, const char *message)
{
	char date[40];
	char clock[8];
	uint32_t center = display->width / 2U;
	uint32_t bottom = display->height;

	if (wallpaper != 0) {
		for (uint32_t row = 0U; row < display->height; row++)
			memcpy(display->framebuffer + (uint64_t)row * display->stride, wallpaper + (uint64_t)row * display->width,
				   (size_t)display->width * sizeof(uint32_t));
	} else {
		(void)login_wallpaper_draw(display);
	}

	login_format_clock(date, clock);
	login_center_label(display, center, display->height / 9U, date, &semibold, 1U, LOGIN_TEXT);

	uint32_t clock_y = display->height / 9U + regular.height + 10U;
	if (wallpaper != 0) {
		login_blur_clock_mask(display, wallpaper, center, clock_y, clock, &login_clock);
	}
	login_center_label(display, center, clock_y, clock, &login_clock, 1U, LOGIN_CLOCK_TEXT);

	uint32_t field_width = display->width < 320U ? display->width - 40U : 280U;
	uint32_t field_left = center - field_width / 2U;
	uint32_t field_top = bottom > 88U ? bottom - 88U : 0U;
	uint32_t field_y = field_top + 14U;
	uint32_t field_color = available ? LOGIN_FIELD_TEXT : LOGIN_FIELD_MUTED;
	const char *placeholder = !available   ? "Connecting securely..."
							  : confirming ? "Confirm Password"
							  : setting	   ? "Create Password"
										   : "Enter Password";
	login_round_rect(display, field_left, field_top, field_width, 44U, 22U, 0x00FFFFFFU);

	if (length != 0U) {
		uint32_t dots = length > 28U ? 28U : length;
		uint32_t dot_x = field_left + 14U;

		for (uint32_t index = 0U; index < dots; index++) {
			login_round_rect(display, dot_x + index * 9U, field_y + 2U, 5U, 5U, 2U, LOGIN_FIELD_TEXT);
		}
	} else {
		login_label(display, field_left + 14U, field_y, placeholder, &semibold, 1U, field_color);
	}

	if (*message != '\0') {
		login_center_label(display, center, field_top + 51U, message, &semibold, 1U, LOGIN_TEXT);
	}

	(void)display_present_full(display);
}

static void
login_thread(void *unused)
{
	(void)unused;

	display_device_t *display = display_primary();

	/* A missing renderer never grants access to the ordinary desktop. */
	if (!display || !display->framebuffer || display->width < 600 || display->height < 360 ||
		display->stride < display->width)
		return;

#if defined(__aarch64__)
	(void)ramfb_console_set_mirroring(false);
#endif
	keyboard_set_secure(true);
	keyboard_flush_keys();
	input_flush();
	kputln("trusted-login: waiting for tepOS authentication");

	uint8_t password[65] = {0}, first[65] = {0};
	uint32_t length = 0, first_length = 0;
	bool setting = false, confirming = false;
	bool repaint = true, availability_known = false;
	tep_result_t previous_available = TEP_ERR_UNAVAILABLE;
	uint64_t painted_minute = UINT64_MAX;
	const char *message = "";

	if (!login_wallpaper_draw(display)) {
		for (uint32_t y = 0U; y < display->height; y++) {
			for (uint32_t x = 0U; x < display->width; x++) {
				display->framebuffer[(uint64_t)y * display->stride + x] = 0x002A3447U;
			}
		}
	}

	uint64_t wallpaper_bytes = (uint64_t)display->width * display->height * sizeof(uint32_t);
	uint32_t *wallpaper = kmalloc((size_t)wallpaper_bytes);
	if (wallpaper != 0)
		for (uint32_t row = 0U; row < display->height; row++)
			memcpy(wallpaper + (uint64_t)row * display->width, display->framebuffer + (uint64_t)row * display->stride,
				   (size_t)display->width * sizeof(uint32_t));
	(void)display_present_full(display);

	for (;;) {
		tep_auth_status_t status = {0};
		tep_result_t available = tep_auth_status(&status);
		uint64_t current_minute = rtc_unix_time() / 60U;

		if (available == TEP_OK) {
			setting = !status.passcode_set;
		}

		if (!availability_known || available != previous_available || current_minute != painted_minute) {
			repaint = true;
		}
		if (repaint) {
			login_paint(display, wallpaper, length, setting, confirming, available == TEP_OK, message);
			repaint = false;
			availability_known = true;
			previous_available = available;
			painted_minute = current_minute;
		}

		bool changed = false;

		for (uint32_t poll = 0; poll < 50 && !changed; poll++) {
			virtio_input_service();
			keyboard_key_t key;

			if (keyboard_take_key(&key)) {
				changed = true;

				if (key.code == 14 && length)
					password[--length] = 0;
				else if (key.code == 1) {
					wipe(password, sizeof(password));
					wipe(first, sizeof(first));
					length = first_length = 0;
					confirming = false;
					message = "";
				} else if (key.code == 28 && available == TEP_OK) {
					uint32_t wait_seconds = 0;
					tep_result_t result;

					if (setting && !confirming) {
						if (length < 4) {
							message = "Use at least four characters.";
							wipe(&key, sizeof(key));
							continue;
						}

						memcpy(first, password, length);
						first_length = length;
						confirming = true;
						message = "";
						wipe(password, sizeof(password));
						length = 0;
						wipe(&key, sizeof(key));
						continue;
					}

					if (setting && (length != first_length || memcmp(first, password, length)))
						result = TEP_ERR_INVALID;
					else if (setting)
						result = tep_auth_set(0, 0, password, length, &wait_seconds);
					else
						result = tep_auth_verify(password, length, &wait_seconds);

					wipe(password, sizeof(password));
					wipe(first, sizeof(first));
					length = first_length = 0;
					confirming = false;

					if (result == TEP_OK) {
						wipe(&key, sizeof(key));
						keyboard_flush_keys();
						input_flush();
						keyboard_set_secure(false);
						ui_service_secure_end();
						if (wallpaper != 0) (void)kfree(wallpaper);
						kputln("trusted-login: unlocked by tepOS");
						return;
					}

					kputln("trusted-login: password refused");
					if (result == TEP_ERR_INVALID) {
						message = "Passwords did not match. Try again.";
					} else if (result == TEP_ERR_DENIED) {
						message = "Incorrect password. Try again.";
					} else {
						message = "Authentication unavailable or delayed. Try again.";
					}
				} else if (key.character >= 32 && key.character < 127 && length < 64) {
					password[length++] = key.character;
					password[length] = 0;
				}

				wipe(&key, sizeof(key));
			}

			(void)sched_sleep_us(10000);
		}
		if (changed) repaint = true;
	}
}

bool
ui_service_trusted_prepare(void)
{
	if (tep_mailbox_link_state() == TEP_LINK_ABSENT) {
		kputln("trusted-login: no enclave link");
		kputln("trusted-login: no login screen");
		return true;
	}

	/* Installed before PID 1 can execute. Neither display claims nor raw input
	 * are available until the enclave accepts the kernel-owned login. */
	if (!ui_service_secure_lock()) return false;

	thread_t thread;
	return kernel_thread_create(proc_task(proc_kernel()), login_thread, 0, &thread) && sched_thread_start(thread);
}
