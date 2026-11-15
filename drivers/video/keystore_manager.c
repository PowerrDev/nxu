/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/keystore_manager.c
 *
 * Render the kernel-owned authentication screen and handle secure input.
 */
#include <drivers/video/ui_service_host.h>
#include <drivers/video/display.h>
#include <drivers/video/login_wallpaper.h>
#include <drivers/video/login_private.h>
#include <drivers/video/setup_assistant.h>
#include <reporter_fonts.h>
#include <drivers/video/login_font.h>
#include <drivers/video/ramfb_console.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/virtio/virtio_input.h>
#include <platform/rtc.h>
#include <frameworks/include/nxu/pointer_cursor.h>
#include <kern/boot/boot_args.h>
#include <kern/console/console.h>
#include <kern/machine/timer.h>
#include <kern/memory/heap.h>
#include <kern/process/thread.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/security/passcode.h>
#include <libk/string.h>
#include <vfs/vfs.h>

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
static uint32_t login_clock_width(const char *text);

uint32_t
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
	uint32_t width = font == &login_clock ? login_clock_width(text) : login_text_width(font, text, 1U);
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
login_frosted_field(display_device_t *display, const uint32_t *wallpaper, uint32_t x, uint32_t y,
					uint32_t width, uint32_t height)
{
	for (uint32_t row = 0U; row < height; row++) {
		for (uint32_t column = 0U; column < width; column++) {
			uint32_t radius = height / 2U;
			int32_t dx = column < radius ? (int32_t)(radius - column) :
				column >= width - radius ? (int32_t)(column - (width - radius - 1U)) : 0;
			int32_t dy = row < radius ? (int32_t)(radius - row) :
				row >= height - radius ? (int32_t)(row - (height - radius - 1U)) : 0;
			if ((uint32_t)(dx * dx + dy * dy) > radius * radius) continue;
			uint32_t base = display->framebuffer[(uint64_t)(y + row) * display->stride + x + column];
			if (wallpaper != 0) {
				uint32_t sum[3] = {0U, 0U, 0U}, samples = 0U;
				for (int32_t oy = -9; oy <= 9; oy += 3) {
					for (int32_t ox = -9; ox <= 9; ox += 3) {
						int32_t sx = (int32_t)(x + column) + ox;
						int32_t sy = (int32_t)(y + row) + oy;
						if (sx < 0 || sy < 0 || sx >= (int32_t)display->width || sy >= (int32_t)display->height) continue;
						uint32_t pixel = wallpaper[(uint64_t)sy * display->width + (uint32_t)sx];
						for (uint32_t channel = 0U; channel < 3U; channel++) sum[channel] += (pixel >> (channel * 8U)) & 255U;
						samples++;
					}
				}
				uint32_t blurred = 0U;
				for (uint32_t channel = 0U; channel < 3U; channel++) blurred |= (sum[channel] / samples) << (channel * 8U);
				base = login_blend(base, blurred, 210U);
			}
			display->framebuffer[(uint64_t)(y + row) * display->stride + x + column] = login_blend(base, 0x00FFFFFFU, 88U);
		}
	}
}

static uint32_t
login_clock_width(const char *text)
{
	uint32_t width = login_text_width(&login_clock, text, 1U);
	uint32_t gaps = 0U;
	while (*text++) gaps++;
	if (gaps != 0U) gaps--;
	return width > gaps ? width - gaps : width;
}

static void
login_clock_label(display_device_t *display, uint32_t center, uint32_t y, const char *text)
{
	uint32_t width = login_clock_width(text);
	uint32_t x = center > width / 2U ? center - width / 2U : 0U;
	while (*text) {
		uint32_t code = (uint8_t)*text++;
		const reporter_glyph_t *glyph = &login_clock.glyphs[(code >= 32U && code <= 126U ? code : '?') - 32U];
		for (uint32_t row = 0U; row < glyph->height; row++) {
			for (uint32_t column = 0U; column < glyph->width; column++) {
				uint32_t alpha = login_clock.pixels[glyph->offset + row * glyph->width + column];
				if (!alpha) continue;
				for (uint32_t bold = 0U; bold < 2U; bold++) {
					int32_t px = (int32_t)x + glyph->x + (int32_t)column + (int32_t)bold;
					int32_t py = (int32_t)y + glyph->y + (int32_t)row;
					if (px < 0 || py < 0 || px >= (int32_t)display->width || py >= (int32_t)display->height) continue;
					uint32_t *pixel = &display->framebuffer[(uint64_t)py * display->stride + (uint32_t)px];
					*pixel = login_blend(*pixel, LOGIN_CLOCK_TEXT, alpha * (LOGIN_CLOCK_TEXT >> 24U) / 255U);
				}
			}
		}
		x += glyph->advance > 1U ? glyph->advance - 1U : glyph->advance;
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

/*
 * The desktop picture chosen in System Settings, which the lock screen shows
 * in place of the built-in one. The choice is the wallpaper preferences file
 * the desktop reads; the picture is a PNG, which the kernel has no decoder
 * for, so the disk carries a QOI copy of every wallpaper for this screen
 * (tools/make_wallpapers.py): <folder>/.login/<name>.qoi beside
 * <folder>/<name>.png. A solid colour is drawn as it is.
 */
#define LOGIN_PREFERENCES "/Volumes/mainOS/Library/Preferences/com.butterscotch.wallpaper.plist"
/* Present once the system volume is mounted, whatever was chosen. */
#define LOGIN_VOLUME_PROBE "/Volumes/mainOS/Library"
#define LOGIN_PREFERENCES_MAX 4096U
#define LOGIN_PICTURE_MAX (16U * 1024U * 1024U)
#define LOGIN_PATH_MAX 256U
/* How long the screen looks for the volume before keeping the built-in picture. */
#define LOGIN_PICTURE_WAIT_US 30000000ULL

typedef enum { LOGIN_CHOSEN_PENDING = 0, LOGIN_CHOSEN_BUILTIN, LOGIN_CHOSEN_DRAWN } login_chosen_t;

/* The whole of a file, at most limit bytes, into a fresh allocation the caller frees; 0 when that fails. */
uint8_t *
login_read_file(const char *path, size_t *size, uint64_t limit)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	vnode_t vnode;
	uint32_t descriptor;

	*size = 0U;
	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return 0;

	uint64_t length = vnode->v_size;

	vnode_rele(vnode);
	if (length == 0ULL || length > limit) return 0;

	uint8_t *bytes = kmalloc((size_t)length);

	if (bytes == 0) return 0;
	if (vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) {
		(void)kfree(bytes);
		return 0;
	}

	uint64_t done = 0ULL;

	while (done < length) {
		uint64_t got = 0ULL;
		uint64_t want = length - done < 65536ULL ? length - done : 65536ULL;

		if (vfs_read(filedesc, descriptor, bytes + done, want, &got) != VFS_STATUS_OK || got == 0ULL) break;
		done += got;
	}
	(void)vfs_close(filedesc, descriptor);

	if (done != length) {
		(void)kfree(bytes);
		return 0;
	}
	*size = (size_t)length;
	return bytes;
}

/*
 * The <string> value of key in the property list text: a flat dictionary,
 * scanned rather than parsed. False when the key is absent, not a string or
 * longer than capacity allows.
 */
bool
login_plist_string(const char *text, size_t length, const char *key, char *value, size_t capacity)
{
	char wanted[40];
	size_t key_length = strlen(key);
	static const char string_tag[] = "<string>";

	if (key_length + 12U > sizeof(wanted)) return false;
	memcpy(wanted, "<key>", 5U);
	memcpy(wanted + 5U, key, key_length);
	memcpy(wanted + 5U + key_length, "</key>", 6U);
	size_t wanted_length = key_length + 11U;

	for (size_t at = 0U; at + wanted_length <= length; at++) {
		if (memcmp(text + at, wanted, wanted_length) != 0) continue;

		size_t cursor = at + wanted_length;

		while (cursor < length && (text[cursor] == ' ' || text[cursor] == '\t' || text[cursor] == '\n' ||
								   text[cursor] == '\r'))
			cursor++;
		if (length - cursor < sizeof(string_tag) - 1U || memcmp(text + cursor, string_tag, sizeof(string_tag) - 1U) != 0)
			return false;
		cursor += sizeof(string_tag) - 1U;

		size_t used = 0U;

		while (cursor < length && text[cursor] != '<') {
			if (used + 1U >= capacity) return false;
			value[used++] = text[cursor++];
		}
		if (cursor >= length) return false;
		while (used > 0U && (value[used - 1U] == ' ' || value[used - 1U] == '\n' || value[used - 1U] == '\t'))
			used--;
		value[used] = '\0';
		return true;
	}
	return false;
}

/* "#RRGGBB" as an XRGB colour. */
static bool
login_parse_color(const char *text, uint32_t *color)
{
	uint32_t value = 0U;

	if (text[0] != '#') return false;
	for (uint32_t index = 1U; index <= 6U; index++) {
		char digit = text[index];
		uint32_t nibble;

		if (digit >= '0' && digit <= '9') {
			nibble = (uint32_t)(digit - '0');
		} else if (digit >= 'a' && digit <= 'f') {
			nibble = (uint32_t)(digit - 'a' + 10);
		} else if (digit >= 'A' && digit <= 'F') {
			nibble = (uint32_t)(digit - 'A' + 10);
		} else {
			return false;
		}
		value = value << 4 | nibble;
	}
	if (text[7] != '\0') return false;
	*color = value;
	return true;
}

/* <folder>/<name>.png to <folder>/.login/<name>.qoi, the copy this screen decodes. */
static bool
login_picture_copy_path(const char *picture, char *path, size_t capacity)
{
	size_t length = strlen(picture);
	size_t slash = length;

	if (length < 5U || picture[0] != '/') return false;
	if (picture[length - 4U] != '.' || (picture[length - 3U] | 0x20) != 'p' || (picture[length - 2U] | 0x20) != 'n' ||
		(picture[length - 1U] | 0x20) != 'g')
		return false;
	while (slash > 0U && picture[slash - 1U] != '/')
		slash--;
	if (slash == 0U || slash >= length - 4U) return false;

	size_t name_length = length - 4U - slash;

	if (slash + 7U + name_length + 4U + 1U > capacity) return false;
	memcpy(path, picture, slash);
	memcpy(path + slash, ".login/", 7U);
	memcpy(path + slash + 7U, picture + slash, name_length);
	memcpy(path + slash + 7U + name_length, ".qoi", 5U);
	return true;
}

/*
 * Draw the picture chosen in System Settings over the whole display, if one
 * was chosen and can be shown. PENDING until the system volume is mounted:
 * the screen comes up before it is.
 */
static login_chosen_t
login_wallpaper_chosen(display_device_t *display)
{
	vnode_t vnode;
	size_t size = 0U;

	if (vfs_lookup(LOGIN_VOLUME_PROBE, &vnode) != VFS_STATUS_OK) return LOGIN_CHOSEN_PENDING;
	vnode_rele(vnode);

	char *text = (char *)login_read_file(LOGIN_PREFERENCES, &size, LOGIN_PREFERENCES_MAX);

	if (text == 0) return LOGIN_CHOSEN_BUILTIN;

	login_chosen_t result = LOGIN_CHOSEN_BUILTIN;
	char value[LOGIN_PATH_MAX];
	char path[LOGIN_PATH_MAX];
	uint32_t color;

	if (login_plist_string(text, size, "Color", value, sizeof(value)) && login_parse_color(value, &color)) {
		for (uint32_t y = 0U; y < display->height; y++) {
			for (uint32_t x = 0U; x < display->width; x++)
				display->framebuffer[(uint64_t)y * display->stride + x] = color;
		}
		result = LOGIN_CHOSEN_DRAWN;
	} else if (login_plist_string(text, size, "Picture", value, sizeof(value)) && value[0] != '\0') {
		size_t picture_size = 0U;
		uint8_t *picture =
			login_picture_copy_path(value, path, sizeof(path)) ? login_read_file(path, &picture_size, LOGIN_PICTURE_MAX) : 0;

		if (picture == 0) {
			kprintf("no lock screen copy of %s: showing the built-in picture\n", value);
		} else if (login_wallpaper_draw_qoi(display, picture, picture + picture_size)) {
			result = LOGIN_CHOSEN_DRAWN;
		} else {
			kprintf("%s is not a picture the lock screen can show\n", path);
			/* It may have drawn part of itself: put the built-in picture back. */
			(void)login_wallpaper_draw(display);
		}
		if (picture != 0) (void)kfree(picture);
	}
	(void)kfree(text);
	return result;
}

/* Keep what is on screen as the wallpaper the screen repaints (and blurs) from. */
static void
login_keep_wallpaper(display_device_t *display, uint32_t *wallpaper)
{
	if (wallpaper == 0) return;
	for (uint32_t row = 0U; row < display->height; row++)
		memcpy(wallpaper + (uint64_t)row * display->width, display->framebuffer + (uint64_t)row * display->stride,
			   (size_t)display->width * sizeof(uint32_t));
}

/*
 * The pointer: mainOS's arrow, as SAP draws it, scaled to the desktop's
 * content scale so it is the size it is on the desktop. It is drawn into the
 * framebuffer in place, keeping what it covers so a move puts that back and
 * presents only the two small rectangles involved.
 */
#define LOGIN_CURSOR_MAX_SCALE 3U
#define LOGIN_CURSOR_MAX_PIXELS \
	(NXU_POINTER_CURSOR_WIDTH * NXU_POINTER_CURSOR_HEIGHT * LOGIN_CURSOR_MAX_SCALE * LOGIN_CURSOR_MAX_SCALE)

static uint32_t login_cursor_under[LOGIN_CURSOR_MAX_PIXELS];
static struct {
	bool known;
	bool shown;
	int32_t x, y;
	uint32_t scale;
} g_login_cursor;

/* The cursor's square on screen for a pointer at (x, y), clipped to the display. */
static bool
login_cursor_bounds(display_device_t *display, int32_t x, int32_t y, int32_t *left, int32_t *top, int32_t *right,
					int32_t *bottom)
{
	int32_t size = (int32_t)(NXU_POINTER_CURSOR_WIDTH * g_login_cursor.scale);

	*left = x - (int32_t)(NXU_POINTER_CURSOR_HOTSPOT_X * g_login_cursor.scale);
	*top = y - (int32_t)(NXU_POINTER_CURSOR_HOTSPOT_Y * g_login_cursor.scale);
	*right = *left + size;
	*bottom = *top + size;
	if (*left < 0) *left = 0;
	if (*top < 0) *top = 0;
	if (*right > (int32_t)display->width) *right = (int32_t)display->width;
	if (*bottom > (int32_t)display->height) *bottom = (int32_t)display->height;
	return *left < *right && *top < *bottom;
}

/* Put back what the cursor covers. */
void
login_cursor_hide(display_device_t *display)
{
	int32_t left, top, right, bottom;

	if (!g_login_cursor.shown) return;
	g_login_cursor.shown = false;
	if (!login_cursor_bounds(display, g_login_cursor.x, g_login_cursor.y, &left, &top, &right, &bottom)) return;

	uint32_t size = NXU_POINTER_CURSOR_WIDTH * g_login_cursor.scale;
	int32_t origin_x = g_login_cursor.x - (int32_t)(NXU_POINTER_CURSOR_HOTSPOT_X * g_login_cursor.scale);
	int32_t origin_y = g_login_cursor.y - (int32_t)(NXU_POINTER_CURSOR_HOTSPOT_Y * g_login_cursor.scale);

	for (int32_t y = top; y < bottom; y++) {
		for (int32_t x = left; x < right; x++) {
			display->framebuffer[(uint64_t)y * display->stride + (uint32_t)x] =
				login_cursor_under[(uint32_t)(y - origin_y) * size + (uint32_t)(x - origin_x)];
		}
	}
}

/* Draw the cursor for a pointer at (x, y), keeping what it covers. */
static void
login_cursor_show(display_device_t *display, int32_t x, int32_t y)
{
	int32_t left, top, right, bottom;

	g_login_cursor.x = x;
	g_login_cursor.y = y;
	if (!login_cursor_bounds(display, x, y, &left, &top, &right, &bottom)) return;

	uint32_t scale = g_login_cursor.scale;
	uint32_t size = NXU_POINTER_CURSOR_WIDTH * scale;
	int32_t origin_x = x - (int32_t)(NXU_POINTER_CURSOR_HOTSPOT_X * scale);
	int32_t origin_y = y - (int32_t)(NXU_POINTER_CURSOR_HOTSPOT_Y * scale);

	for (int32_t py = top; py < bottom; py++) {
		uint32_t row = (uint32_t)(py - origin_y);

		for (int32_t px = left; px < right; px++) {
			uint32_t column = (uint32_t)(px - origin_x);
			uint32_t *pixel = &display->framebuffer[(uint64_t)py * display->stride + (uint32_t)px];

			login_cursor_under[row * size + column] = *pixel;
			*pixel = nxu_pointer_cursor_blend(
				*pixel, nxu_pointer_cursor_pixels[(row / scale) * NXU_POINTER_CURSOR_WIDTH + column / scale]);
		}
	}
	g_login_cursor.shown = true;
}

/* Follow the pointer to (x, y): put back the old square, draw the new, present both. */
static void
login_cursor_move(display_device_t *display, int32_t x, int32_t y)
{
	int32_t old_left = 0, old_top = 0, old_right = 0, old_bottom = 0;
	int32_t left, top, right, bottom;
	bool had = g_login_cursor.shown &&
			   login_cursor_bounds(display, g_login_cursor.x, g_login_cursor.y, &old_left, &old_top, &old_right, &old_bottom);

	login_cursor_hide(display);
	login_cursor_show(display, x, y);
	if (!login_cursor_bounds(display, x, y, &left, &top, &right, &bottom)) {
		if (!had) return;
		left = old_left;
		top = old_top;
		right = old_right;
		bottom = old_bottom;
	} else if (had) {
		if (old_left < left) left = old_left;
		if (old_top < top) top = old_top;
		if (old_right > right) right = old_right;
		if (old_bottom > bottom) bottom = old_bottom;
	}
	(void)display_present(display, (uint32_t)left, (uint32_t)top, (uint32_t)(right - left), (uint32_t)(bottom - top));
}

/* The content scale (ui.scale), for the island's size. */
static uint32_t g_login_scale_permille = 1000U;

static void
login_paint(display_device_t *display, const uint32_t *wallpaper, uint32_t length, bool setting, bool confirming,
			bool available, const char *message)
{
	char date[40];
	char clock[8];
	uint32_t center = display->width / 2U;
	uint32_t bottom = display->height;

	/* Everything is painted over, the cursor too: it is drawn again last. */
	g_login_cursor.shown = false;
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
	login_clock_label(display, center, clock_y, clock);

	uint32_t field_width = display->width < 320U ? display->width - 40U : 280U;
	uint32_t field_left = center - field_width / 2U;
	uint32_t field_top = bottom > 84U ? bottom - 84U : 0U;
	uint32_t field_height = 38U;
	uint32_t field_y = field_top + (field_height - regular.height) / 2U;
	uint32_t field_color = available ? LOGIN_FIELD_TEXT : LOGIN_FIELD_MUTED;
	const char *placeholder = !available   ? "Starting up..."
							  : confirming ? "Confirm Password"
							  : setting	   ? "Create Password"
										   : "Enter Password";
	login_frosted_field(display, wallpaper, field_left, field_top, field_width, field_height);

	if (length != 0U) {
		uint32_t dots = length > 28U ? 28U : length;
		uint32_t dot_x = field_left + 14U;
		uint32_t dot_size = 7U;
		uint32_t dot_y = field_top + (field_height - dot_size) / 2U;

		for (uint32_t index = 0U; index < dots; index++) {
			login_round_rect(display, dot_x + index * 9U, dot_y, dot_size, dot_size, dot_size / 2U, LOGIN_FIELD_TEXT);
		}
	} else {
		login_label(display, field_left + 14U, field_y, placeholder, &semibold, 1U, field_color);
	}

	if (*message == '\0' && available && setting) {
		message = confirming ? "Enter the same password again." : "Choose a password to protect groveOS.";
	}
	if (*message != '\0') {
		login_center_label(display, center, field_top + 51U, message, &semibold, 1U, LOGIN_TEXT);
	}

	setup_assistant_draw_island(display, g_login_scale_permille);
	if (g_login_cursor.known) login_cursor_show(display, g_login_cursor.x, g_login_cursor.y);
	(void)display_present_full(display);
}

/* The faces login_private.h names. */
static const reporter_font_t *
login_face(login_face_t face)
{
	switch (face) {
	case LOGIN_FACE_SEMIBOLD:
		return &semibold;
	case LOGIN_FACE_TITLE:
		return &login_title;
	case LOGIN_FACE_HEADING:
		return &login_heading;
	case LOGIN_FACE_SMALL:
		return &login_small;
	case LOGIN_FACE_SMALL_BOLD:
		return &login_small_bold;
	case LOGIN_FACE_REGULAR:
	default:
		return &regular;
	}
}

uint32_t
login_face_width(login_face_t face, const char *text, uint32_t scale)
{
	return login_text_width(login_face(face), text, scale);
}

uint32_t
login_face_height(login_face_t face, uint32_t scale)
{
	return login_face(face)->height * scale;
}

void
login_face_draw(display_device_t *display, int32_t x, int32_t y, const char *text, login_face_t face, uint32_t scale,
				uint32_t argb)
{
	if (x < 0 || y < 0 || (argb >> 24U) == 0U) return;
	login_label(display, (uint32_t)x, (uint32_t)y, text, login_face(face), scale, argb);
}

void
login_face_draw_clipped(display_device_t *display, int32_t x, int32_t y, const char *text, login_face_t face,
						uint32_t scale, uint32_t argb, int32_t left, int32_t top, int32_t right, int32_t bottom)
{
	const reporter_font_t *font = login_face(face);
	uint32_t opacity = argb >> 24U;

	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > (int32_t)display->width) right = (int32_t)display->width;
	if (bottom > (int32_t)display->height) bottom = (int32_t)display->height;
	if (opacity == 0U || left >= right || top >= bottom || y >= bottom || y + (int32_t)(font->height * scale) * 2 < top) return;

	while (*text) {
		uint32_t code = (uint8_t)*text++;
		const reporter_glyph_t *glyph = &font->glyphs[(code >= 32U && code <= 126U ? code : '?') - 32U];
		int32_t glyph_left = x + glyph->x * (int32_t)scale;

		/* Only glyphs that reach into the clip: a repaint of a small rect skips the rest of the line. */
		if (glyph_left < right && glyph_left + (int32_t)(glyph->width * scale) > left) {
			for (uint32_t row = 0U; row < glyph->height; row++) {
				for (uint32_t column = 0U; column < glyph->width; column++) {
					uint32_t alpha = font->pixels[glyph->offset + row * glyph->width + column] * opacity / 255U;
					if (alpha == 0U) continue;

					for (uint32_t sy = 0U; sy < scale; sy++) {
						int32_t py = y + (glyph->y + (int32_t)row) * (int32_t)scale + (int32_t)sy;
						if (py < top || py >= bottom) continue;
						for (uint32_t sx = 0U; sx < scale; sx++) {
							int32_t px = glyph_left + (int32_t)(column * scale + sx);
							if (px < left || px >= right) continue;
							uint32_t *pixel = &display->framebuffer[(uint64_t)py * display->stride + (uint32_t)px];
							*pixel = login_blend(*pixel, argb, alpha);
						}
					}
				}
			}
		}
		x += glyph->advance * scale;
	}
}

bool
login_pointer_poll(display_device_t *display, int32_t *x, int32_t *y, uint32_t *pressed)
{
	*pressed = 0U;
	if (!g_login_cursor.known || !ui_service_secure_pointer(x, y, pressed)) return false;
	if (*x != g_login_cursor.x || *y != g_login_cursor.y) login_cursor_move(display, *x, *y);
	return true;
}

void
login_cursor_show_again(display_device_t *display)
{
	if (g_login_cursor.known && !g_login_cursor.shown) login_cursor_show(display, g_login_cursor.x, g_login_cursor.y);
}

void
login_cursor_present(display_device_t *display)
{
	int32_t left, top, right, bottom;

	if (g_login_cursor.shown &&
		login_cursor_bounds(display, g_login_cursor.x, g_login_cursor.y, &left, &top, &right, &bottom))
		(void)display_present(display, (uint32_t)left, (uint32_t)top, (uint32_t)(right - left),
							  (uint32_t)(bottom - top));
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
	kprintf("waiting for authentication.\n");

	uint8_t password[65] = {0}, first[65] = {0};
	uint32_t length = 0, first_length = 0;
	bool setting = false, confirming = false;
	bool repaint = true, availability_known = false, set_up = false, welcome_cleared = false;
	passcode_result_t previous_available = PASSCODE_ERR_UNAVAILABLE;
	uint64_t painted_minute = UINT64_MAX;
	const char *message = "";

	if (!login_wallpaper_draw(display)) {
		for (uint32_t y = 0U; y < display->height; y++) {
			for (uint32_t x = 0U; x < display->width; x++) {
				display->framebuffer[(uint64_t)y * display->stride + x] = 0x002A3447U;
			}
		}
	}

	/* The chosen picture over the built-in one, if the system volume is already there. */
	uint64_t chosen_deadline = timer_get_microseconds() + LOGIN_PICTURE_WAIT_US;
	login_chosen_t chosen = login_wallpaper_chosen(display);
	bool chosen_known = chosen != LOGIN_CHOSEN_PENDING;

	uint64_t wallpaper_bytes = (uint64_t)display->width * display->height * sizeof(uint32_t);
	uint32_t *wallpaper = kmalloc((size_t)wallpaper_bytes);
	login_keep_wallpaper(display, wallpaper);

	/* The pointer starts in the middle of the screen, as on the desktop. */
	uint32_t scale_permille = boot_arg_uint32("ui.scale", 2000U);
	uint32_t cursor_scale = (scale_permille + 500U) / 1000U;
	g_login_scale_permille = scale_permille;
	g_login_cursor.scale = cursor_scale < 1U ? 1U : cursor_scale > LOGIN_CURSOR_MAX_SCALE ? LOGIN_CURSOR_MAX_SCALE
																						 : cursor_scale;
	g_login_cursor.shown = false;
	{
		int32_t pointer_x, pointer_y;
		uint32_t pressed;

		g_login_cursor.known = ui_service_secure_pointer(&pointer_x, &pointer_y, &pressed);
		if (g_login_cursor.known) login_cursor_show(display, pointer_x, pointer_y);
	}
	(void)display_present_full(display);

	for (;;) {
		/* The system volume mounts after the screen is up: look for the chosen picture until it has. */
		if (!chosen_known) {
			chosen = login_wallpaper_chosen(display);
			chosen_known = chosen != LOGIN_CHOSEN_PENDING || timer_get_microseconds() >= chosen_deadline;
			if (chosen == LOGIN_CHOSEN_DRAWN) login_keep_wallpaper(display, wallpaper);
			/* Whatever it drew (a picture that failed part way is drawn over), paint the screen again. */
			if (chosen != LOGIN_CHOSEN_PENDING) repaint = true;
		}

		passcode_status_t status = {0};
		passcode_result_t available = passcode_status(&status);
		uint64_t current_minute = rtc_unix_time() / 60U;

		if (available == PASSCODE_OK) {
			setting = !status.passcode_set;
			if (status.locked && *message == '\0') {
				message = "Password locked. Reset it from Recovery.";
				repaint = true;
			}
		}

		/*
		 * A first boot (no password yet) is the setup assistant, not the lock
		 * screen: it ends with the password set, which unlocks. If it cannot
		 * finish, the lock screen's own "create a password" takes over.
		 */
		if (available == PASSCODE_OK && setting && !set_up) {
			set_up = true;
			if (setup_assistant_run(display, wallpaper, scale_permille)) {
				keyboard_flush_keys();
				input_flush();
				keyboard_set_secure(false);
				ui_service_secure_end();
				if (wallpaper != 0) (void)kfree(wallpaper);
				return;
			}
			keyboard_flush_keys();
			repaint = true;
		}

		/* Every later boot: the desktop's one-time welcome has been seen. */
		if (available == PASSCODE_OK && !setting && !welcome_cleared) welcome_cleared = setup_assistant_forget_welcome();

		if (!availability_known || available != previous_available || current_minute != painted_minute) {
			repaint = true;
		}
		if (repaint) {
			login_paint(display, wallpaper, length, setting, confirming, available == PASSCODE_OK, message);
			repaint = false;
			availability_known = true;
			previous_available = available;
			painted_minute = current_minute;
		}

		bool changed = false;

		for (uint32_t poll = 0; poll < 50 && !changed; poll++) {
			virtio_input_service();

			int32_t pointer_x, pointer_y;
			uint32_t pressed;

			if (g_login_cursor.known && ui_service_secure_pointer(&pointer_x, &pointer_y, &pressed) &&
				(pointer_x != g_login_cursor.x || pointer_y != g_login_cursor.y))
				login_cursor_move(display, pointer_x, pointer_y);

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
				} else if (key.code == 28 && available == PASSCODE_OK) {
					uint32_t wait_seconds = 0;
					passcode_result_t result;

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
						result = PASSCODE_ERR_INVALID;
					else if (setting)
						result = passcode_set(0, 0, password, length, &wait_seconds);
					else
						result = passcode_verify(password, length, &wait_seconds);

					wipe(password, sizeof(password));
					wipe(first, sizeof(first));
					length = first_length = 0;
					confirming = false;

					if (result == PASSCODE_OK) {
						wipe(&key, sizeof(key));
						keyboard_flush_keys();
						input_flush();
						keyboard_set_secure(false);
						ui_service_secure_end();
						if (wallpaper != 0) (void)kfree(wallpaper);
						return;
					}

					kprintf("password refused: %s\n", passcode_result_name(result));

					if (result == PASSCODE_ERR_INVALID) {
						message = "Passwords did not match. Try again.";
					} else if (result == PASSCODE_ERR_DENIED) {
						message = "Incorrect password. Try again.";
					} else if (result == PASSCODE_ERR_RETRY_LATER) {
						message = wait_seconds > 60U ? "Too many attempts. Try again in a few minutes."
													 : "Too many attempts. Try again in a minute.";
					} else if (result == PASSCODE_ERR_LOCKED) {
						message = "Password locked. Reset it from Recovery.";
					} else {
						message = "The password could not be checked. Try again.";
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
	/* Installed before PID 1 can execute. Neither display claims nor raw input
	 * are available until the passcode (kern/security/passcode.c) is accepted,
	 * or set for the first time. */
	if (!ui_service_secure_lock()) return false;

	thread_t thread;
	return kernel_thread_create(proc_task(proc_kernel()), login_thread, 0, &thread) && sched_thread_start(thread);
}
