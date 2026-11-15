/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/setup_assistant.c
 *
 * The first-boot setup assistant. See setup_assistant.h.
 *
 * It runs where the lock screen would (keystore_manager.c), before any
 * process: it draws into the framebuffer itself, reads the keyboard and the
 * pointer itself, and writes the choices straight to the system volume.
 *
 *   hello      "hello" written stroke by stroke over the wallpaper, the pen
 *              slowing through the turns (login_hello_strokes.h, generated
 *              from tools/hello.svg), then Get Started.
 *   the card   a white card with continuous (G2) corners over the blurred
 *              wallpaper, one step at a time: the account, the device's
 *              name, the Dock (with a live preview), the menu bar clock (with
 *              a live clock), the wallpaper.
 *   apply      the password into tepOS (that is what unlocks), the account
 *              into /etc/passwd, group and shadow, the name into
 *              /etc/hostname, the rest as the preferences System Settings
 *              writes.
 *   goodbye    "hello" again, mainOS's this time, and Finish.
 *
 * Everything is integer (no FPU use in the kernel). Positions are in pixels;
 * sizes are given in points and scaled by the screen's content scale.
 */
#include <drivers/video/setup_assistant.h>

#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/tep/tep_crypto.h>
#include <drivers/video/login_hello_strokes.h>
#include <drivers/video/login_private.h>
#include <drivers/video/login_wallpaper.h>
#include <drivers/video/setup_icons.h>
#include <drivers/virtio/virtio_input.h>
#include <kern/console/console.h>
#include <kern/machine/timer.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <libk/sha256.h>
#include <libk/string.h>
#include <platform/rtc.h>
#include <vfs/vfs.h>

/* ---- where things go on disk ------------------------------------------ */

#define SA_VOLUME "/Volumes/mainOS"
#define SA_PASSWD SA_VOLUME "/etc/passwd"
#define SA_GROUP SA_VOLUME "/etc/group"
#define SA_SHADOW SA_VOLUME "/etc/shadow"
#define SA_HOSTNAME SA_VOLUME "/etc/hostname"
#define SA_PREFERENCES SA_VOLUME "/Library/Preferences/"
#define SA_DOCK_PLIST SA_PREFERENCES "com.nxu.dock.plist"
#define SA_CLOCK_PLIST SA_PREFERENCES "com.butterscotch.menuextra.clock.plist"
#define SA_WALLPAPER_PLIST SA_PREFERENCES "com.butterscotch.wallpaper.plist"
#define SA_TIMED_PLIST SA_PREFERENCES "com.nxu.timed.plist"
/* Read by the desktop: its one-time welcome in the Dynamic Island. */
#define SA_WELCOME_PLIST SA_PREFERENCES "com.butterscotch.setup.plist"
#define SA_PHOTOS_DIR SA_VOLUME "/Library/Caches/Wallpapers"
#define SA_SYSTEM_PICTURES_DIR SA_VOLUME "/System/Library/Desktop Pictures"
/* The picture built into the kernel, and the one chosen unless another is. */
#define SA_DEFAULT_PICTURE "GoldenGate"

#define SA_ACCOUNT_UID "1000"
#define SA_PBKDF2_ITERATIONS 4096U
#define SA_SALT_BYTES 16U

/* ---- timing ------------------------------------------------------------- */

#define SA_FRAME_US 16000ULL
#define SA_WRITE_DELAY_US 500000ULL
#define SA_WRITE_US 4200000ULL
#define SA_PROMPT_AT_US (SA_WRITE_DELAY_US + SA_WRITE_US + 300000ULL)
#define SA_PROMPT_FADE_US 700000ULL
#define SA_LEAVE_US 600000ULL
#define SA_TO_CARD_US 900000ULL
#define SA_PAGE_US 420000ULL
#define SA_CONTROL_US 260000ULL
#define SA_DOCK_US 520000ULL
#define SA_TO_DONE_US 700000ULL
#define SA_CARET_US 530000ULL

/* ---- colours (ARGB) ----------------------------------------------------- */

#define SA_WHITE 0xFFFFFFFFU
#define SA_INK 0xFF1D1D1FU
#define SA_SECONDARY 0xFF6E6E73U
#define SA_PLACEHOLDER 0xFFAEAEB2U
#define SA_BORDER 0xFFC7C7CCU
#define SA_FILL 0xFFF2F2F7U
#define SA_SEPARATOR 0xFFE5E5EAU
#define SA_ACCENT 0xFF0A84FFU
#define SA_ACCENT_DIM 0xFF9CC4F5U
#define SA_FOCUS 0x700A84FFU
#define SA_GREEN 0xFF34C759U
#define SA_RED 0xFFFF3B30U
#define SA_SCRIM 96U

#define SA_SUB 256
#define SA_ROW_MAX 4096U
#define SA_CORNER_MAX 112U
#define SA_PICTURES_MAX 6U
#define SA_HITS_MAX 40U

typedef struct {
	int32_t left, top, right, bottom;
} sa_rect_t;

typedef enum {
	SA_PAGE_ACCOUNT,
	SA_PAGE_DEVICE,
	SA_PAGE_DOCK,
	SA_PAGE_MENUBAR,
	SA_PAGE_WALLPAPER,
	SA_PAGES,
	SA_PAGE_APPLY = SA_PAGES,
} sa_page_t;

typedef enum {
	SA_HELLO,	/* "hello", then Get Started */
	SA_TO_CARD, /* the picture blurs, the card comes up */
	SA_CARD,	/* the steps */
	SA_TO_DONE, /* the card goes, the picture sharpens */
	SA_DONE,	/* mainOS says hello, then Finish */
	SA_LEAVE,	/* everything fades */
} sa_phase_t;

enum {
	SA_HIT_BACK = 1,
	SA_HIT_CONTINUE,
	SA_HIT_GET_STARTED,
	SA_HIT_FINISH,
	SA_HIT_SEGMENT = 10, /* + 0..2 */
	SA_HIT_TOGGLE = 20,	 /* + 0..5: Dock recents, Dock trash, the four clock options */
	SA_HIT_PICTURE = 30, /* + picture */
	SA_HIT_FIELD = 100,	 /* + field */
};

typedef struct {
	char text[72];
	uint32_t length;
	uint32_t capacity;
	bool secure;
} sa_field_t;

enum { SA_FIELD_USERNAME, SA_FIELD_PASSWORD, SA_FIELD_VERIFY, SA_FIELD_DEVICE, SA_FIELDS };

typedef struct {
	char name[48];
	const char *directory;
	uint32_t *thumbnail;
} sa_picture_t;

typedef struct {
	sa_rect_t rect;
	int32_t id;
} sa_hit_t;

typedef struct {
	int32_t hello_x, hello_y;
	uint32_t hello_width, hello_height;
	int32_t button_x, button_y;
	uint32_t button_radius;
	int32_t caption_y;
	sa_rect_t band;
} sa_hello_layout_t;

/* The Dock's three sizes, in points (System Settings' tilesize). */
static const uint32_t sa_dock_sizes[3] = {36U, 48U, 64U};
static const char *const sa_dock_size_names[3] = {"Small", "Medium", "Large"};
static const char *const sa_clock_option_names[4] = {"Show the day of the week", "Show the date", "Use a 24-hour clock",
													 "Show seconds"};

static struct {
	display_device_t *display;
	uint32_t *wallpaper; /* sharp, display->width per row */
	uint32_t *backdrop;	 /* blurred and tinted, the same size; null: none */
	uint32_t permille;
	uint32_t font_scale;
	int32_t width, height;
	sa_rect_t clip;
	uint32_t mix; /* the background: 0 the dimmed picture, 255 the blurred one */

	sa_phase_t phase;
	uint64_t phase_at;
	uint32_t written;	   /* how much of "hello" is on screen */
	uint32_t shown_prompt; /* the prompt's opacity on screen */
	uint32_t card_alpha;
	int32_t card_rise;
	uint32_t hello_alpha; /* the hello, fading under the card as it comes up */
	bool repaint_all;	  /* the picture changed: the whole screen, not the card */

	sa_page_t page, from_page;
	uint64_t page_at;
	uint32_t focus; /* the field typed into */
	const char *error;
	uint64_t caret_at;

	sa_field_t fields[SA_FIELDS];
	uint32_t dock_size, dock_size_from;
	uint64_t dock_at;
	bool toggles[6];
	uint64_t toggle_at[6];
	uint64_t segment_at;
	uint32_t segment_from;

	sa_picture_t pictures[SA_PICTURES_MAX];
	uint32_t picture_count;
	int32_t picture; /* -1: the built-in picture */
	bool pictures_loaded;
	int32_t clock_offset;
	uint64_t clock_second;

	sa_hit_t hits[SA_HITS_MAX];
	uint32_t hit_count;
	bool hits_live;
	uint32_t pressed;

	/*
	 * The card at rest, without its contents (the blurred picture behind,
	 * its shadow, border and body), kept once drawn: a frame starts from a
	 * copy of it instead of drawing them again.
	 */
	uint32_t *card_base;
	sa_rect_t card_base_area;
	bool card_base_valid;
	/*
	 * Where the settled page's moving parts are (recorded as it is drawn), so
	 * a caret blink, a switch, the Dock preview or the clock repaints only
	 * itself. Empty: not known, repaint the card.
	 */
	sa_rect_t blink_rect;
	sa_rect_t switch_rects[6];
	sa_rect_t dock_rect;
	sa_rect_t clock_rect;
} g_sa;

static uint8_t g_sa_row[SA_ROW_MAX];

static struct {
	uint32_t extent;
	uint8_t coverage[SA_CORNER_MAX * SA_CORNER_MAX];
} g_sa_corners[4];
static uint32_t g_sa_corner_next;

static void
sa_wipe(void *memory, size_t size)
{
	volatile uint8_t *bytes = memory;

	while (size--)
		*bytes++ = 0;
}

/* ---- arithmetic --------------------------------------------------------- */

/*
 * background towards foreground by alpha (of 255), XRGB: red and blue in one
 * multiply, green in another, divided by 255 with a shift (x + x / 256 + 1)
 * / 256 instead of three divisions. Every pixel this screen paints goes
 * through here.
 */
static inline uint32_t
sa_blend(uint32_t background, uint32_t foreground, uint32_t alpha)
{
	uint32_t keep = 255U - alpha;
	uint32_t rb = (background & 0x00FF00FFU) * keep + (foreground & 0x00FF00FFU) * alpha + 0x00800080U;
	uint32_t g = (background & 0x0000FF00U) * keep + (foreground & 0x0000FF00U) * alpha + 0x00008000U;

	rb = ((rb + ((rb >> 8U) & 0x00FF00FFU)) >> 8U) & 0x00FF00FFU;
	g = ((g + ((g >> 8U) & 0x0000FF00U)) >> 8U) & 0x0000FF00U;
	return rb | g;
}

static int32_t
sa_pt(int32_t points)
{
	return (int32_t)((int64_t)points * g_sa.permille / 1000);
}

static uint32_t
sa_isqrt(uint32_t value)
{
	if (value < 2U) return value;
	uint32_t x = value, y = (value >> 1) + 1U;
	while (y < x) {
		x = y;
		y = (x + value / x) / 2U;
	}
	return x;
}

static int32_t
sa_floor_div(int32_t value, int32_t divisor)
{
	return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

static int32_t
sa_min(int32_t a, int32_t b)
{
	return a < b ? a : b;
}

static int32_t
sa_max(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

/* Ease-in-out cubic, 0 to 65536 over duration. */
static uint32_t
sa_ease_in_out(uint64_t elapsed, uint64_t duration)
{
	if (elapsed >= duration) return 65536U;
	uint64_t t = elapsed * 65536U / duration;
	if (t < 32768U) return (uint32_t)(4U * t * t / 65536U * t / 65536U);
	uint64_t u = 65536U - t;
	return (uint32_t)(65536U - 4U * u * u / 65536U * u / 65536U);
}

/* Smoothstep, 0 to 65536 over duration: the pen's run. */
static uint32_t
sa_smoothstep(uint64_t elapsed, uint64_t duration)
{
	if (elapsed >= duration) return 65536U;
	uint64_t t = elapsed * 65536U / duration;
	return (uint32_t)(((t * t) >> 16) * (3U * 65536U - 2U * t) >> 16);
}

/* Ease-out cubic, 0 to 255 over duration. */
static uint32_t
sa_ease_out(uint64_t elapsed, uint64_t duration)
{
	if (elapsed >= duration) return 255U;
	uint64_t inverse = duration - elapsed;
	uint64_t cubed = inverse * inverse / duration * inverse / duration;
	return 255U - (uint32_t)(cubed * 255U / duration);
}

/*
 * Ease-out with a little overshoot ("back"), 0 to 65536 over duration, a
 * touch over 65536 on the way: things that grow settle into place.
 */
static int32_t
sa_ease_back(uint64_t elapsed, uint64_t duration)
{
	if (elapsed >= duration) return 65536;
	/* 1 + c3 (t - 1)^3 + c1 (t - 1)^2, c1 = 1.2, c3 = c1 + 1. */
	int64_t t = (int64_t)(elapsed * 65536U / duration) - 65536;
	int64_t squared = t * t / 65536;
	int64_t cubed = squared * t / 65536;
	return (int32_t)(65536 + cubed * 22 / 10 + squared * 12 / 10);
}

static uint32_t
sa_fade(uint32_t argb, uint32_t alpha)
{
	return (((argb >> 24U) * alpha / 255U) << 24U) | (argb & 0x00FFFFFFU);
}

static uint32_t
sa_mix_color(uint32_t from, uint32_t to, uint32_t amount)
{
	uint32_t alpha_from = from >> 24U, alpha_to = to >> 24U;
	uint32_t alpha = (alpha_from * (255U - amount) + alpha_to * amount) / 255U;

	return (alpha << 24U) | (sa_blend(from, to, amount) & 0x00FFFFFFU);
}

/* ---- rectangles ----------------------------------------------------------- */

static sa_rect_t
sa_rect(int32_t x, int32_t y, int32_t width, int32_t height)
{
	return (sa_rect_t){x, y, x + width, y + height};
}

static sa_rect_t
sa_inset(sa_rect_t rect, int32_t by)
{
	return (sa_rect_t){rect.left + by, rect.top + by, rect.right - by, rect.bottom - by};
}

static sa_rect_t
sa_intersect(sa_rect_t a, sa_rect_t b)
{
	sa_rect_t out = {sa_max(a.left, b.left), sa_max(a.top, b.top), sa_min(a.right, b.right), sa_min(a.bottom, b.bottom)};

	if (out.right < out.left) out.right = out.left;
	if (out.bottom < out.top) out.bottom = out.top;
	return out;
}

static bool
sa_contains(sa_rect_t rect, int32_t x, int32_t y)
{
	return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

static bool
sa_empty(sa_rect_t rect)
{
	return rect.left >= rect.right || rect.top >= rect.bottom;
}

static sa_rect_t
sa_union(sa_rect_t a, sa_rect_t b)
{
	if (sa_empty(a)) return b;
	if (sa_empty(b)) return a;
	return (sa_rect_t){sa_min(a.left, b.left), sa_min(a.top, b.top), sa_max(a.right, b.right), sa_max(a.bottom, b.bottom)};
}

/* Note a moving part's place, while the settled page is drawn. */
static void
sa_record(sa_rect_t *slot, sa_rect_t rect)
{
	if (g_sa.hits_live) *slot = sa_union(*slot, rect);
}

static sa_rect_t
sa_screen(void)
{
	return (sa_rect_t){0, 0, g_sa.width, g_sa.height};
}

static void
sa_set_clip(sa_rect_t rect)
{
	g_sa.clip = sa_intersect(rect, sa_screen());
}

/* ---- pixels ------------------------------------------------------------- */

static inline void
sa_put(int32_t x, int32_t y, uint32_t argb, uint32_t coverage)
{
	if (x < g_sa.clip.left || x >= g_sa.clip.right || y < g_sa.clip.top || y >= g_sa.clip.bottom) return;

	uint32_t alpha = (argb >> 24U) * coverage / 255U;

	if (alpha == 0U) return;

	uint32_t *pixel = &g_sa.display->framebuffer[(uint64_t)y * g_sa.display->stride + (uint32_t)x];

	*pixel = alpha >= 255U ? (argb & 0x00FFFFFFU) : sa_blend(*pixel, argb, alpha);
}

static void
sa_fill(sa_rect_t rect, uint32_t argb)
{
	rect = sa_intersect(rect, g_sa.clip);
	for (int32_t y = rect.top; y < rect.bottom; y++)
		for (int32_t x = rect.left; x < rect.right; x++)
			sa_put(x, y, argb, 255U);
}

/* The background at (x, y): the dimmed picture and the blurred one, mixed by g_sa.mix. */
static inline uint32_t
sa_background(int32_t x, int32_t y)
{
	uint64_t at = (uint64_t)y * (uint32_t)g_sa.width + (uint32_t)x;
	uint32_t dimmed = g_sa.wallpaper != 0 ? sa_blend(g_sa.wallpaper[at], 0U, SA_SCRIM) : 0x001C222CU;

	if (g_sa.mix == 0U || g_sa.backdrop == 0) return dimmed;
	if (g_sa.mix >= 255U) return g_sa.backdrop[at];
	return sa_blend(dimmed, g_sa.backdrop[at], g_sa.mix);
}

static void
sa_restore(sa_rect_t rect)
{
	rect = sa_intersect(rect, g_sa.clip);
	for (int32_t y = rect.top; y < rect.bottom; y++) {
		uint32_t *row = &g_sa.display->framebuffer[(uint64_t)y * g_sa.display->stride];

		for (int32_t x = rect.left; x < rect.right; x++)
			row[x] = sa_background(x, y);
	}
}

static void sa_capsule(sa_rect_t rect, uint32_t argb);

/*
 * The Dynamic Island at rest, as the desktop shows it (island.rs): a black
 * capsule 96 x 30 points, 2 below the top of the screen, with a faint
 * hairline to find its edge on a dark picture. Nothing happens to it here.
 */
static void
sa_island(void)
{
	sa_rect_t island = sa_rect(g_sa.width / 2 - sa_pt(48), sa_pt(2), sa_pt(96), sa_pt(30));

	if (sa_empty(sa_intersect(sa_inset(island, -2), g_sa.clip))) return;
	sa_capsule(island, 0x1AFFFFFFU);
	sa_capsule(sa_inset(island, 1), 0xFF000000U);
}

/* Show rect: paint is done, the island and the cursor go back over it, and both reach the screen. */
static void
sa_present(sa_rect_t rect)
{
	rect = sa_intersect(rect, sa_screen());
	g_sa.clip = rect;
	sa_island();
	login_cursor_show_again(g_sa.display);
	if (!sa_empty(rect))
		(void)display_present(g_sa.display, (uint32_t)rect.left, (uint32_t)rect.top,
							  (uint32_t)(rect.right - rect.left), (uint32_t)(rect.bottom - rect.top));
	login_cursor_present(g_sa.display);
}

/* ---- continuous corners --------------------------------------------------- */

/*
 * A continuous ("squircle") corner: a superellipse quarter, x^4 + y^4 = 1,
 * over a box 1.5 times the nominal radius. It leaves the straight edge with
 * zero curvature (curvature continuous, G2), where a circular arc jumps. The
 * coverage of each of its pixels (16 samples) is kept for a few sizes.
 */
static const uint8_t *
sa_corner(uint32_t extent)
{
	for (uint32_t index = 0U; index < 4U; index++)
		if (g_sa_corners[index].extent == extent) return g_sa_corners[index].coverage;

	uint32_t slot = g_sa_corner_next++ % 4U;
	uint8_t *coverage = g_sa_corners[slot].coverage;
	const uint64_t one = 4096ULL * 4096ULL * 4096ULL * 4096ULL;

	for (uint32_t cy = 0U; cy < extent; cy++) {
		for (uint32_t cx = 0U; cx < extent; cx++) {
			uint32_t inside = 0U;

			for (uint32_t sy = 0U; sy < 4U; sy++) {
				for (uint32_t sx = 0U; sx < 4U; sx++) {
					/* From the corner's inner point, as a fraction of the extent, in 1/4096. */
					int64_t u = ((int64_t)extent * 8 - (cx * 8 + sx * 2 + 1)) * 4096 / ((int64_t)extent * 8);
					int64_t v = ((int64_t)extent * 8 - (cy * 8 + sy * 2 + 1)) * 4096 / ((int64_t)extent * 8);

					if (u <= 0 || v <= 0) {
						inside++;
						continue;
					}
					uint64_t u4 = (uint64_t)(u * u) * (uint64_t)(u * u), v4 = (uint64_t)(v * v) * (uint64_t)(v * v);
					if (u4 + v4 <= one) inside++;
				}
			}
			coverage[cy * extent + cx] = (uint8_t)(inside * 255U / 16U);
		}
	}
	g_sa_corners[slot].extent = extent;
	return coverage;
}

static uint32_t
sa_corner_extent(sa_rect_t rect, uint32_t radius)
{
	uint32_t extent = radius * 3U / 2U;
	uint32_t half_width = (uint32_t)(rect.right - rect.left) / 2U, half_height = (uint32_t)(rect.bottom - rect.top) / 2U;

	if (extent > half_width) extent = half_width;
	if (extent > half_height) extent = half_height;
	if (extent > SA_CORNER_MAX) extent = SA_CORNER_MAX;
	return extent;
}

/* How much of pixel (x, y) the shape covers (of 255). */
static uint32_t
sa_squircle_coverage(sa_rect_t rect, uint32_t extent, const uint8_t *corner, int32_t x, int32_t y)
{
	if (!sa_contains(rect, x, y)) return 0U;

	int32_t cx = x - rect.left < (int32_t)extent ? x - rect.left : rect.right - 1 - x < (int32_t)extent ? rect.right - 1 - x : -1;
	int32_t cy = y - rect.top < (int32_t)extent ? y - rect.top : rect.bottom - 1 - y < (int32_t)extent ? rect.bottom - 1 - y : -1;

	return cx < 0 || cy < 0 ? 255U : corner[(uint32_t)cy * extent + (uint32_t)cx];
}

static void
sa_squircle(sa_rect_t rect, uint32_t radius, uint32_t argb)
{
	if (sa_empty(rect) || (argb >> 24U) == 0U) return;

	uint32_t extent = sa_corner_extent(rect, radius);
	const uint8_t *corner = sa_corner(extent);
	sa_rect_t area = sa_intersect(rect, g_sa.clip);

	for (int32_t y = area.top; y < area.bottom; y++) {
		bool corner_row = y - rect.top < (int32_t)extent || rect.bottom - 1 - y < (int32_t)extent;

		for (int32_t x = area.left; x < area.right; x++)
			sa_put(x, y, argb, corner_row ? sa_squircle_coverage(rect, extent, corner, x, y) : 255U);
	}
}

/* A ring `thickness` wide just outside rect, following its corners. */
static void
sa_squircle_ring(sa_rect_t rect, uint32_t radius, int32_t thickness, uint32_t argb)
{
	sa_rect_t outer = sa_inset(rect, -thickness);
	uint32_t outer_extent = sa_corner_extent(outer, radius + (uint32_t)thickness);
	const uint8_t *outer_corner = sa_corner(outer_extent);
	uint32_t inner_extent = sa_corner_extent(rect, radius);
	const uint8_t *inner_corner = sa_corner(inner_extent);
	sa_rect_t area = sa_intersect(outer, g_sa.clip);

	for (int32_t y = area.top; y < area.bottom; y++) {
		for (int32_t x = area.left; x < area.right; x++) {
			uint32_t out = sa_squircle_coverage(outer, outer_extent, outer_corner, x, y);
			uint32_t in = sa_squircle_coverage(rect, inner_extent, inner_corner, x, y);

			if (out > in) sa_put(x, y, argb, out - in);
		}
	}
}

/*
 * A soft shadow under rect: darkest (peak) along its edge, falling off
 * quadratically over spread, dropped by drop. Only drawn outside rect, which
 * the opaque card covers.
 */
static void
sa_shadow(sa_rect_t rect, int32_t radius, int32_t spread, int32_t drop, uint32_t peak)
{
	sa_rect_t shadow = {rect.left, rect.top + drop, rect.right, rect.bottom + drop};
	sa_rect_t area = sa_intersect(sa_inset(shadow, -spread), g_sa.clip);

	if (peak == 0U || spread <= 0) return;
	for (int32_t y = area.top; y < area.bottom; y++) {
		for (int32_t x = area.left; x < area.right; x++) {
			/* Under the card, except its corners (which do not fill their box). */
			if (sa_contains(rect, x, y)) {
				int32_t corner = radius * 3 / 2;
				bool in_corner_column = x - rect.left < corner || rect.right - 1 - x < corner;
				bool in_corner_row = y - rect.top < corner || rect.bottom - 1 - y < corner;

				if (!in_corner_row) {
					x = rect.right - 1;
					continue;
				}
				if (!in_corner_column) {
					x = rect.right - corner - 1;
					continue;
				}
			}

			int32_t dx = x < shadow.left + radius ? shadow.left + radius - x : x >= shadow.right - radius ? x - (shadow.right - radius - 1) : 0;
			int32_t dy = y < shadow.top + radius ? shadow.top + radius - y : y >= shadow.bottom - radius ? y - (shadow.bottom - radius - 1) : 0;
			int32_t distance = (int32_t)sa_isqrt((uint32_t)(dx * dx + dy * dy)) - radius;

			if (distance >= spread) continue;

			uint32_t alpha = peak;
			if (distance > 0) {
				uint32_t remaining = (uint32_t)(spread - distance);
				alpha = peak * remaining * remaining / (uint32_t)(spread * spread);
			}
			sa_put(x, y, alpha << 24U, 255U);
		}
	}
}

/* ---- round-capped strokes (the pen, circles, capsules, arrows) ----------- */

/* The distance to (x, y), 0 well inside, -1 well outside. */
static int32_t
sa_point_distance(int32_t x, int32_t y, int32_t inner_squared, int32_t outer_squared)
{
	if (x > 46000 || x < -46000 || y > 46000 || y < -46000) return -1;
	int32_t squared = x * x + y * y;
	if (squared <= inner_squared) return 0;
	if (squared >= outer_squared) return -1;
	return (int32_t)sa_isqrt((uint32_t)squared);
}

/*
 * Add a round-capped piece from a to b, radius thick, to the row buffer, whose
 * first pixel's left edge is row_x and whose centre line is centre_y (all in
 * 1/256 px). A one-pixel ramp across the edge; overlaps keep the larger.
 */
static void
sa_cover_piece(uint8_t *row, int32_t span, int32_t row_x, int32_t centre_y, int32_t ax, int32_t ay, int32_t bx, int32_t by,
			   int32_t radius)
{
	int32_t reach = radius + SA_SUB / 2;
	if (centre_y < sa_min(ay, by) - reach || centre_y > sa_max(ay, by) + reach) return;

	int32_t first = sa_max(sa_floor_div(sa_min(ax, bx) - reach - row_x, SA_SUB), 0);
	int32_t last = sa_min(sa_floor_div(sa_max(ax, bx) + reach - row_x, SA_SUB), span - 1);
	if (first > last) return;

	int32_t dx = bx - ax, dy = by - ay;
	int32_t length_squared = dx * dx + dy * dy;
	int32_t length = (int32_t)sa_isqrt((uint32_t)length_squared);
	int32_t py = centre_y - ay;
	int32_t inner = radius > SA_SUB / 2 ? radius - SA_SUB / 2 : 0;

	for (int32_t column = first; column <= last; column++) {
		int32_t px = row_x + column * SA_SUB + SA_SUB / 2 - ax;
		int32_t along = px * dx + py * dy;
		int32_t distance;

		if (length == 0 || along <= 0) {
			distance = sa_point_distance(px, py, inner * inner, reach * reach);
		} else if (along >= length_squared) {
			distance = sa_point_distance(px - dx, py - dy, inner * inner, reach * reach);
		} else {
			int32_t cross = px * dy - py * dx;
			distance = (cross < 0 ? -cross : cross) / length;
		}
		if (distance < 0) continue;

		int32_t coverage = radius + SA_SUB / 2 - distance;
		if (coverage <= 0) continue;
		if (coverage > 255) coverage = 255;
		if ((uint8_t)coverage > row[column]) row[column] = (uint8_t)coverage;
	}
}

static void
sa_blend_row(int32_t span, int32_t left, int32_t y, uint32_t argb)
{
	for (int32_t index = 0; index < span; index++)
		if (g_sa_row[index] != 0U) sa_put(left + index, y, argb, g_sa_row[index]);
}

/* Straight round-capped pieces {ax, ay, bx, by} (1/256 px), radius (1/256 px) thick. */
static void
sa_stroke(const int32_t (*pieces)[4], uint32_t count, int32_t radius, uint32_t argb)
{
	int32_t reach = radius + SA_SUB;
	int32_t min_x = 0x7FFFFFFF, min_y = 0x7FFFFFFF, max_x = -0x7FFFFFFF, max_y = -0x7FFFFFFF;

	if (count == 0U || (argb >> 24U) == 0U) return;
	for (uint32_t index = 0U; index < count; index++) {
		min_x = sa_min(min_x, sa_min(pieces[index][0], pieces[index][2]));
		min_y = sa_min(min_y, sa_min(pieces[index][1], pieces[index][3]));
		max_x = sa_max(max_x, sa_max(pieces[index][0], pieces[index][2]));
		max_y = sa_max(max_y, sa_max(pieces[index][1], pieces[index][3]));
	}

	sa_rect_t area = sa_intersect((sa_rect_t){sa_floor_div(min_x - reach, SA_SUB), sa_floor_div(min_y - reach, SA_SUB),
											  (max_x + reach) / SA_SUB + 2, (max_y + reach) / SA_SUB + 2},
								  g_sa.clip);
	area.right = sa_min(area.right, area.left + (int32_t)SA_ROW_MAX);
	int32_t span = area.right - area.left;

	for (int32_t y = area.top; y < area.bottom; y++) {
		memset(g_sa_row, 0, (size_t)span);
		for (uint32_t index = 0U; index < count; index++)
			sa_cover_piece(g_sa_row, span, area.left * SA_SUB, y * SA_SUB + SA_SUB / 2, pieces[index][0], pieces[index][1],
						   pieces[index][2], pieces[index][3], radius);
		sa_blend_row(span, area.left, y, argb);
	}
}

/* A filled anti-aliased circle centred on (x, y) px (1/256), radius (1/256). */
static void
sa_circle(int32_t x, int32_t y, int32_t radius, uint32_t argb)
{
	const int32_t piece[1][4] = {{x, y, x, y}};

	sa_stroke(piece, 1U, radius - SA_SUB / 2, argb);
}

/* A capsule filling rect (its ends round). */
static void
sa_capsule(sa_rect_t rect, uint32_t argb)
{
	int32_t height = rect.bottom - rect.top, radius = height * SA_SUB / 2;
	int32_t y = rect.top * SA_SUB + radius;
	const int32_t piece[1][4] = {{rect.left * SA_SUB + radius, y, rect.right * SA_SUB - radius, y}};

	sa_stroke(piece, 1U, radius - SA_SUB / 2, argb);
}

/* ---- text ----------------------------------------------------------------- */

static uint32_t
sa_text_width(login_face_t face, const char *text)
{
	return login_face_width(face, text, g_sa.font_scale);
}

static int32_t
sa_text_height(login_face_t face)
{
	return (int32_t)login_face_height(face, g_sa.font_scale);
}

static void
sa_text(login_face_t face, const char *text, int32_t x, int32_t y, uint32_t argb)
{
	login_face_draw_clipped(g_sa.display, x, y, text, face, g_sa.font_scale, argb, g_sa.clip.left, g_sa.clip.top,
							g_sa.clip.right, g_sa.clip.bottom);
}

static void
sa_text_centered(login_face_t face, const char *text, int32_t centre_x, int32_t y, uint32_t argb)
{
	sa_text(face, text, centre_x - (int32_t)sa_text_width(face, text) / 2, y, argb);
}

/* "GoldenGate" as "Golden Gate". */
static void
sa_pretty_name(const char *name, char *out, size_t capacity)
{
	size_t used = 0U;

	for (size_t index = 0U; name[index] != '\0' && used + 2U < capacity; index++) {
		char character = name[index];

		if (index > 0U && character >= 'A' && character <= 'Z' && name[index - 1U] >= 'a' && name[index - 1U] <= 'z')
			out[used++] = ' ';
		out[used++] = character == '_' || character == '-' ? ' ' : character;
	}
	out[used] = '\0';
}

/* ---- the hit list ----------------------------------------------------------- */

static void
sa_hit(sa_rect_t rect, int32_t id)
{
	if (g_sa.hits_live && g_sa.hit_count < SA_HITS_MAX) g_sa.hits[g_sa.hit_count++] = (sa_hit_t){rect, id};
}

static int32_t
sa_hit_at(int32_t x, int32_t y)
{
	for (uint32_t index = g_sa.hit_count; index > 0U; index--)
		if (sa_contains(g_sa.hits[index - 1U].rect, x, y)) return g_sa.hits[index - 1U].id;
	return 0;
}

/* ---- the blurred backdrop --------------------------------------------------- */

/*
 * The picture, blurred (an eighth of the size, three box passes each way,
 * back up bilinearly) and a little darkened: what the card sits on.
 */
static void
sa_build_backdrop(void)
{
	uint32_t width = (uint32_t)g_sa.width, height = (uint32_t)g_sa.height;
	uint32_t low_width = (width + 7U) / 8U, low_height = (height + 7U) / 8U;

	if (g_sa.wallpaper == 0) return;
	if (g_sa.backdrop == 0) g_sa.backdrop = kmalloc((size_t)width * height * sizeof(uint32_t));

	uint32_t *low = kmalloc((size_t)low_width * low_height * sizeof(uint32_t) * 2U);

	if (g_sa.backdrop == 0 || low == 0) {
		if (low != 0) (void)kfree(low);
		if (g_sa.backdrop != 0) (void)kfree(g_sa.backdrop);
		g_sa.backdrop = 0;
		return;
	}
	uint32_t *temp = low + low_width * low_height;

	for (uint32_t y = 0U; y < low_height; y++) {
		for (uint32_t x = 0U; x < low_width; x++) {
			uint32_t sum[3] = {0U, 0U, 0U}, count = 0U;

			for (uint32_t sy = y * 8U; sy < y * 8U + 8U && sy < height; sy += 2U) {
				for (uint32_t sx = x * 8U; sx < x * 8U + 8U && sx < width; sx += 2U) {
					uint32_t pixel = g_sa.wallpaper[(uint64_t)sy * width + sx];
					for (uint32_t channel = 0U; channel < 3U; channel++) sum[channel] += (pixel >> (channel * 8U)) & 255U;
					count++;
				}
			}
			uint32_t pixel = 0U;
			for (uint32_t channel = 0U; channel < 3U; channel++) pixel |= (sum[channel] / (count ? count : 1U)) << (channel * 8U);
			low[y * low_width + x] = pixel;
		}
	}

	for (uint32_t pass = 0U; pass < 6U; pass++) {
		bool across = (pass & 1U) == 0U;
		uint32_t *from = across ? low : temp, *to = across ? temp : low;
		uint32_t lines = across ? low_height : low_width, length = across ? low_width : low_height;

		for (uint32_t line = 0U; line < lines; line++) {
			for (uint32_t index = 0U; index < length; index++) {
				uint32_t sum[3] = {0U, 0U, 0U};

				for (int32_t offset = -2; offset <= 2; offset++) {
					int32_t at = sa_min(sa_max((int32_t)index + offset, 0), (int32_t)length - 1);
					uint32_t pixel = across ? from[line * low_width + (uint32_t)at] : from[(uint32_t)at * low_width + line];
					for (uint32_t channel = 0U; channel < 3U; channel++) sum[channel] += (pixel >> (channel * 8U)) & 255U;
				}
				uint32_t pixel = 0U;
				for (uint32_t channel = 0U; channel < 3U; channel++) pixel |= (sum[channel] / 5U) << (channel * 8U);
				to[across ? line * low_width + index : index * low_width + line] = pixel;
			}
		}
	}

	for (uint32_t y = 0U; y < height; y++) {
		int32_t v = (int32_t)((y * 2U + 1U) * 128U / 8U) - 128;
		v = sa_min(sa_max(v, 0), ((int32_t)low_height - 1) * 256);
		uint32_t row0 = (uint32_t)v >> 8, fy = (uint32_t)v & 255U, row1 = row0 + 1U < low_height ? row0 + 1U : row0;

		for (uint32_t x = 0U; x < width; x++) {
			int32_t u = (int32_t)((x * 2U + 1U) * 128U / 8U) - 128;
			u = sa_min(sa_max(u, 0), ((int32_t)low_width - 1) * 256);
			uint32_t column0 = (uint32_t)u >> 8, fx = (uint32_t)u & 255U;
			uint32_t column1 = column0 + 1U < low_width ? column0 + 1U : column0;
			uint32_t top = sa_blend(low[row0 * low_width + column0], low[row0 * low_width + column1], fx);
			uint32_t bottom = sa_blend(low[row1 * low_width + column0], low[row1 * low_width + column1], fx);

			g_sa.backdrop[(uint64_t)y * width + x] = sa_blend(sa_blend(top, bottom, fy), 0U, 70U);
		}
	}
	(void)kfree(low);
}

/* ---- pictures ------------------------------------------------------------- */

/* Decode a QOI picture over a width x height buffer, scaled to cover it. */
static bool
sa_decode_picture(const char *path, uint32_t *pixels, uint32_t width, uint32_t height)
{
	size_t size = 0U;
	uint8_t *data = login_read_file(path, &size, 16U * 1024U * 1024U);

	if (data == 0) return false;

	display_device_t target = {0};
	target.framebuffer = pixels;
	target.width = width;
	target.height = height;
	target.stride = width;
	target.bytes_per_pixel = 4U;

	bool drawn = login_wallpaper_draw_qoi(&target, data, data + size);

	(void)kfree(data);
	return drawn;
}

static bool
sa_join(char *out, size_t capacity, const char *a, const char *b, const char *c, const char *d)
{
	const char *parts[4] = {a, b, c, d};
	size_t used = 0U;

	for (uint32_t index = 0U; index < 4U; index++) {
		for (const char *character = parts[index]; character != 0 && *character != '\0'; character++) {
			if (used + 1U >= capacity) return false;
			out[used++] = *character;
		}
	}
	out[used] = '\0';
	return true;
}

static uint32_t
sa_thumbnail_width(void)
{
	return (uint32_t)sa_pt(156);
}

static uint32_t
sa_thumbnail_height(void)
{
	return (uint32_t)sa_pt(98);
}

/*
 * The pictures there are to choose from: each has a QOI copy for the lock
 * screen in .login next to it (tools/make_wallpapers.py), which is what this
 * decodes. GoldenGate first.
 */
static void
sa_load_pictures(void)
{
	static const char *const directories[2] = {SA_PHOTOS_DIR, SA_SYSTEM_PICTURES_DIR};
	filedesc_t filedesc = &proc_kernel()->p_fd;

	if (g_sa.pictures_loaded) return;
	g_sa.pictures_loaded = true;

	for (uint32_t index = 0U; index < 2U; index++) {
		char path[160];
		uint32_t descriptor;

		if (!sa_join(path, sizeof(path), directories[index], "/.login", 0, 0) ||
			vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK)
			continue;

		vfs_dirent_t entry;

		while (g_sa.picture_count < SA_PICTURES_MAX && vfs_readdir(filedesc, descriptor, &entry) == VFS_STATUS_OK) {
			size_t length = strlen(entry.name);

			if (length <= 4U || length - 4U >= sizeof(g_sa.pictures[0].name) || entry.name[0] == '.' ||
				memcmp(entry.name + length - 4U, ".qoi", 4U) != 0)
				continue;

			sa_picture_t *picture = &g_sa.pictures[g_sa.picture_count++];

			memcpy(picture->name, entry.name, length - 4U);
			picture->name[length - 4U] = '\0';
			picture->directory = directories[index];
			picture->thumbnail = 0;
		}
		(void)vfs_close(filedesc, descriptor);
	}

	/* The default first, and chosen. */
	for (uint32_t index = 0U; index < g_sa.picture_count; index++) {
		if (strcmp(g_sa.pictures[index].name, SA_DEFAULT_PICTURE) == 0) {
			sa_picture_t chosen = g_sa.pictures[index];

			memmove(&g_sa.pictures[1], &g_sa.pictures[0], index * sizeof(sa_picture_t));
			g_sa.pictures[0] = chosen;
			if (g_sa.picture < 0) g_sa.picture = 0;
			break;
		}
	}

	uint32_t thumbnail_width = sa_thumbnail_width(), thumbnail_height = sa_thumbnail_height();

	for (uint32_t index = 0U; index < g_sa.picture_count; index++) {
		sa_picture_t *picture = &g_sa.pictures[index];
		char path[200];

		picture->thumbnail = kmalloc((size_t)thumbnail_width * thumbnail_height * sizeof(uint32_t));
		if (picture->thumbnail == 0) continue;
		if (!sa_join(path, sizeof(path), picture->directory, "/.login/", picture->name, ".qoi") ||
			!sa_decode_picture(path, picture->thumbnail, thumbnail_width, thumbnail_height)) {
			(void)kfree(picture->thumbnail);
			picture->thumbnail = 0;
		}
	}
	kprintf("%u pictures to choose from\n", (unsigned)g_sa.picture_count);
}

/* Make picture index the screen's picture: decoded at full size, blurred again. */
static void
sa_choose_picture(int32_t index)
{
	char path[200];
	sa_picture_t *picture = &g_sa.pictures[index];

	if (index == g_sa.picture || g_sa.wallpaper == 0) return;
	if (!sa_join(path, sizeof(path), picture->directory, "/.login/", picture->name, ".qoi") ||
		!sa_decode_picture(path, g_sa.wallpaper, (uint32_t)g_sa.width, (uint32_t)g_sa.height)) {
		kprintf("could not show %s\n", picture->name);
		return;
	}
	g_sa.picture = index;
	sa_build_backdrop();
	g_sa.repaint_all = true;
	g_sa.card_base_valid = false;
}

/*
 * Draw source (source_width x source_height) into rect, scaled to cover it,
 * inside rect's continuous corners. A null source draws the screen's picture.
 */
static void
sa_picture_in(sa_rect_t rect, uint32_t radius, const uint32_t *source, uint32_t source_width, uint32_t source_height,
			  uint32_t alpha, bool top_only)
{
	int32_t width = rect.right - rect.left, height = rect.bottom - rect.top;

	if (width <= 0 || height <= 0) return;
	if (source == 0) {
		source = g_sa.wallpaper;
		source_width = (uint32_t)g_sa.width;
		source_height = (uint32_t)g_sa.height;
	}

	uint32_t extent = sa_corner_extent(rect, radius);
	const uint8_t *corner = sa_corner(extent);
	sa_rect_t area = sa_intersect(rect, g_sa.clip);
	/* Cover: the larger scale, centred (or from the top, for the menu bar's strip). */
	uint64_t scale_x = (uint64_t)source_width * 65536U / (uint32_t)width;
	uint64_t scale_y = (uint64_t)source_height * 65536U / (uint32_t)height;
	uint64_t scale = scale_x < scale_y ? scale_x : scale_y;
	uint32_t offset_x = (uint32_t)((source_width * 65536ULL - (uint64_t)width * scale) / 2U >> 16);
	uint32_t offset_y = top_only ? 0U : (uint32_t)((source_height * 65536ULL - (uint64_t)height * scale) / 2U >> 16);

	for (int32_t y = area.top; y < area.bottom; y++) {
		uint32_t sy = offset_y + (uint32_t)(((uint64_t)(y - rect.top) * scale) >> 16);
		if (sy >= source_height) sy = source_height - 1U;

		for (int32_t x = area.left; x < area.right; x++) {
			uint32_t sx = offset_x + (uint32_t)(((uint64_t)(x - rect.left) * scale) >> 16);
			if (sx >= source_width) sx = source_width - 1U;

			uint32_t coverage = sa_squircle_coverage(rect, extent, corner, x, y);
			uint32_t color = source != 0 ? source[(uint64_t)sy * source_width + sx] : 0x002A3447U;

			sa_put(x, y, (alpha << 24U) | (color & 0x00FFFFFFU), coverage);
		}
	}
}

/* ---- the hello -------------------------------------------------------------- */

static sa_hello_layout_t
sa_hello_layout(void)
{
	sa_hello_layout_t layout;
	uint32_t width = (uint32_t)sa_pt(520);

	if (width > (uint32_t)g_sa.width * 62U / 100U) width = (uint32_t)g_sa.width * 62U / 100U;
	if (width > SA_ROW_MAX) width = SA_ROW_MAX;
	layout.hello_width = width;
	layout.hello_height = (uint32_t)((uint64_t)width * LOGIN_HELLO_UNITS_HEIGHT / LOGIN_HELLO_UNITS_WIDTH);
	layout.hello_x = (g_sa.width - (int32_t)width) / 2;
	layout.hello_y = g_sa.height / 2 - (int32_t)layout.hello_height / 2 - sa_pt(56);
	layout.button_radius = (uint32_t)sa_pt(22);
	layout.button_x = g_sa.width / 2;
	layout.button_y = layout.hello_y + (int32_t)layout.hello_height + sa_pt(110);
	layout.caption_y = layout.button_y + (int32_t)layout.button_radius + sa_pt(12);
	layout.band = (sa_rect_t){0, sa_max(layout.hello_y - 4, 0), g_sa.width,
							  sa_min(layout.caption_y + sa_text_height(LOGIN_FACE_SEMIBOLD) + sa_pt(16), g_sa.height)};
	return layout;
}

static int64_t
sa_hello_multiplier(uint32_t width)
{
	return ((int64_t)width * SA_SUB << 16) / LOGIN_HELLO_UNITS_WIDTH;
}

static int32_t
sa_hello_to_sub(int32_t units, int64_t multiplier)
{
	return (int32_t)(((int64_t)units * multiplier) >> 16);
}

/*
 * How much of the word is written `progress` (of 65536) through the run: the
 * pen's timetable (login_hello_times) slows it through the turns.
 */
static uint32_t
sa_hello_reveal(uint32_t progress)
{
	uint32_t time = (uint32_t)((uint64_t)LOGIN_HELLO_TIME_TOTAL * (progress > 65536U ? 65536U : progress) >> 16);
	uint32_t low = 0U, high = LOGIN_HELLO_POINTS - 1U;

	while (low + 1U < high) {
		uint32_t middle = (low + high) / 2U;
		if (login_hello_times[middle] <= time) low = middle;
		else high = middle;
	}
	if (time >= login_hello_times[LOGIN_HELLO_POINTS - 1U]) return LOGIN_HELLO_LENGTH;

	uint32_t from = login_hello_times[low], to = login_hello_times[low + 1U];
	uint32_t start = login_hello_lengths[low], end = login_hello_lengths[low + 1U];

	if (to <= from) return start;
	return start + (uint32_t)((uint64_t)(end - start) * (time - from) / (to - from));
}

/* Draw the first `reveal` units of the word, white at alpha, inside the clip. */
static void
sa_hello_word(const sa_hello_layout_t *layout, uint32_t reveal, uint32_t alpha)
{
	sa_rect_t area = sa_intersect(sa_rect(layout->hello_x, layout->hello_y, (int32_t)layout->hello_width,
										  (int32_t)layout->hello_height),
								  g_sa.clip);

	if (reveal == 0U || alpha == 0U || sa_empty(area)) return;
	if (reveal > LOGIN_HELLO_LENGTH) reveal = LOGIN_HELLO_LENGTH;

	int64_t multiplier = sa_hello_multiplier(layout->hello_width);
	int32_t radius = sa_max(sa_hello_to_sub(LOGIN_HELLO_STROKE / 2, multiplier), SA_SUB / 2);
	int32_t span = area.right - area.left;
	int32_t row_x = (area.left - layout->hello_x) * SA_SUB;
	int64_t reach = LOGIN_HELLO_STROKE / 2 + 4;
	int64_t left_units = ((int64_t)row_x << 16) / multiplier;
	int64_t right_units = ((int64_t)(area.right - layout->hello_x) * SA_SUB << 16) / multiplier;

	for (int32_t y = area.top; y < area.bottom; y++) {
		int32_t centre_y = (y - layout->hello_y) * SA_SUB + SA_SUB / 2;
		int64_t row_units = ((int64_t)centre_y << 16) / multiplier;

		memset(g_sa_row, 0, (size_t)span);
		for (uint32_t chunk = 0U; chunk < LOGIN_HELLO_CHUNKS; chunk++) {
			uint32_t first = login_hello_chunks[chunk].first, last = login_hello_chunks[chunk].last;

			if (row_units < login_hello_chunks[chunk].min_y - reach || row_units > login_hello_chunks[chunk].max_y + reach ||
				right_units < login_hello_chunks[chunk].min_x - reach || left_units > login_hello_chunks[chunk].max_x + reach ||
				login_hello_lengths[first] >= reveal)
				continue;

			for (uint32_t index = first; index < last; index++) {
				uint32_t start = login_hello_lengths[index], end = login_hello_lengths[index + 1U];
				if (start >= reveal) break;

				int32_t ax = sa_hello_to_sub(login_hello_points[index][0], multiplier);
				int32_t ay = sa_hello_to_sub(login_hello_points[index][1], multiplier);
				int32_t bx = sa_hello_to_sub(login_hello_points[index + 1U][0], multiplier);
				int32_t by = sa_hello_to_sub(login_hello_points[index + 1U][1], multiplier);

				/* The pen is somewhere along this piece. */
				if (end > reveal && end > start) {
					bx = ax + (int32_t)((int64_t)(bx - ax) * (int32_t)(reveal - start) / (int32_t)(end - start));
					by = ay + (int32_t)((int64_t)(by - ay) * (int32_t)(reveal - start) / (int32_t)(end - start));
				}
				sa_cover_piece(g_sa_row, span, row_x, centre_y, ax, ay, bx, by, radius);
			}
		}
		sa_blend_row(span, area.left, y, (alpha << 24U) | 0x00FFFFFFU);
	}
}

/* The box around what is written between from and to, with room for the stroke. */
static sa_rect_t
sa_hello_written(const sa_hello_layout_t *layout, uint32_t from, uint32_t to)
{
	int64_t multiplier = sa_hello_multiplier(layout->hello_width);
	int32_t min_x = 0x7FFFFFFF, min_y = 0x7FFFFFFF, max_x = -0x7FFFFFFF, max_y = -0x7FFFFFFF;

	for (uint32_t index = 0U; index + 1U < LOGIN_HELLO_POINTS; index++) {
		if (login_hello_lengths[index + 1U] < from || login_hello_lengths[index] > to) continue;
		for (uint32_t end = index; end <= index + 1U; end++) {
			min_x = sa_min(min_x, login_hello_points[end][0]);
			min_y = sa_min(min_y, login_hello_points[end][1]);
			max_x = sa_max(max_x, login_hello_points[end][0]);
			max_y = sa_max(max_y, login_hello_points[end][1]);
		}
	}
	if (min_x > max_x) return (sa_rect_t){0, 0, 0, 0};

	int32_t pad = LOGIN_HELLO_STROKE / 2 + 8;
	return (sa_rect_t){layout->hello_x + sa_floor_div(sa_hello_to_sub(min_x - pad, multiplier), SA_SUB) - 1,
					   layout->hello_y + sa_floor_div(sa_hello_to_sub(min_y - pad, multiplier), SA_SUB) - 1,
					   layout->hello_x + sa_hello_to_sub(max_x + pad, multiplier) / SA_SUB + 2,
					   layout->hello_y + sa_hello_to_sub(max_y + pad, multiplier) / SA_SUB + 2};
}

/* What appears under the word: Get Started first, mainOS's welcome and Finish at the end. */
static void
sa_hello_prompt(const sa_hello_layout_t *layout, uint32_t alpha, bool finish)
{
	if (alpha == 0U) return;

	if (!finish) {
		int32_t radius = (int32_t)layout->button_radius;
		int32_t cx = layout->button_x * SA_SUB + SA_SUB / 2, cy = layout->button_y * SA_SUB + SA_SUB / 2;
		int32_t half = radius * SA_SUB * 36 / 100, head = half * 62 / 100;
		const int32_t arrow[3][4] = {
			{cx - half, cy, cx + half, cy},
			{cx + half - head, cy - head, cx + half, cy},
			{cx + half - head, cy + head, cx + half, cy},
		};

		sa_circle(cx, cy, radius * SA_SUB, sa_fade(0x3AFFFFFFU, alpha));
		sa_stroke(arrow, 3U, radius * SA_SUB / 22, sa_fade(SA_WHITE, alpha));
		sa_text_centered(LOGIN_FACE_SEMIBOLD, "Get Started", layout->button_x, layout->caption_y,
						 sa_fade(0xDCFFFFFFU, alpha));
		sa_hit(sa_inset(sa_rect(layout->button_x - radius, layout->button_y - radius, radius * 2, radius * 2), -sa_pt(8)),
			   SA_HIT_GET_STARTED);
		return;
	}

	char line[96];
	const char *name = g_sa.fields[SA_FIELD_USERNAME].text;

	(void)sa_join(line, sizeof(line), "Welcome to mainOS, ", name, ".", 0);
	int32_t line_y = layout->hello_y + (int32_t)layout->hello_height + sa_pt(36);
	sa_text_centered(LOGIN_FACE_HEADING, line, g_sa.width / 2, line_y, sa_fade(0xF0FFFFFFU, alpha));

	sa_rect_t button = sa_rect(g_sa.width / 2 - sa_pt(64), line_y + sa_text_height(LOGIN_FACE_HEADING) + sa_pt(30),
							   sa_pt(128), sa_pt(36));
	sa_capsule(button, sa_fade(0xF2FFFFFFU, alpha));
	sa_text_centered(LOGIN_FACE_SEMIBOLD, "Finish", g_sa.width / 2,
					 button.top + (button.bottom - button.top - sa_text_height(LOGIN_FACE_SEMIBOLD)) / 2,
					 sa_fade(SA_INK, alpha));
	sa_hit(sa_inset(button, -sa_pt(6)), SA_HIT_FINISH);
}

/* Repaint rect of a hello screen. */
static void
sa_hello_frame(const sa_hello_layout_t *layout, sa_rect_t rect, uint32_t reveal, uint32_t alpha, uint32_t prompt, bool finish)
{
	sa_set_clip(sa_intersect(rect, layout->band));
	if (sa_empty(g_sa.clip)) return;
	login_cursor_hide(g_sa.display);
	sa_restore(g_sa.clip);
	sa_hello_word(layout, reveal, alpha);
	g_sa.hit_count = 0U;
	g_sa.hits_live = prompt == 255U && alpha == 255U;
	sa_hello_prompt(layout, prompt * alpha / 255U, finish);
	g_sa.hits_live = false;
	sa_present(g_sa.clip);
}

/* ---- the card's pieces -------------------------------------------------------- */

static sa_rect_t
sa_card(void)
{
	int32_t width = sa_min(sa_pt(640), g_sa.width - sa_pt(32));
	int32_t height = sa_min(sa_pt(500), g_sa.height - sa_pt(32));

	return sa_rect((g_sa.width - width) / 2, (g_sa.height - height) / 2 + g_sa.card_rise, width, height);
}

/* Everything a card frame can touch: the card, its shadow, and room to rise. */
static sa_rect_t
sa_card_area(void)
{
	sa_rect_t card = sa_card();

	return (sa_rect_t){card.left - sa_pt(48), card.top - sa_pt(48), card.right + sa_pt(48), card.bottom + sa_pt(80)};
}

static void
sa_button(sa_rect_t rect, const char *label, bool primary, bool enabled, uint32_t alpha, int32_t id)
{
	uint32_t fill = primary ? (enabled ? SA_ACCENT : SA_ACCENT_DIM) : SA_SEPARATOR;

	sa_capsule(rect, sa_fade(fill, alpha));
	sa_text_centered(LOGIN_FACE_SEMIBOLD, label, (rect.left + rect.right) / 2,
					 rect.top + (rect.bottom - rect.top - sa_text_height(LOGIN_FACE_SEMIBOLD)) / 2,
					 sa_fade(primary ? SA_WHITE : SA_INK, alpha));
	if (enabled) sa_hit(rect, id);
}

static void
sa_field_draw(sa_rect_t rect, uint32_t index, const char *placeholder, uint32_t alpha, uint64_t now)
{
	sa_field_t *field = &g_sa.fields[index];
	bool focused = g_sa.page == SA_PAGE_ACCOUNT ? g_sa.focus == index : index == SA_FIELD_DEVICE;
	uint32_t radius = (uint32_t)sa_pt(7);
	int32_t text_x = rect.left + sa_pt(10);
	int32_t text_y = rect.top + (rect.bottom - rect.top - sa_text_height(LOGIN_FACE_REGULAR)) / 2;
	int32_t end_x = text_x;

	if (focused) sa_squircle_ring(rect, radius, sa_max(sa_pt(3), 2), sa_fade(SA_FOCUS, alpha));
	sa_squircle(rect, radius, sa_fade(focused ? 0xFF7EB6F7U : SA_BORDER, alpha));
	sa_squircle(sa_inset(rect, 1), radius > 0U ? radius - 1U : 0U, sa_fade(SA_WHITE, alpha));

	if (field->length == 0U) {
		sa_text(LOGIN_FACE_REGULAR, placeholder, text_x, text_y, sa_fade(SA_PLACEHOLDER, alpha));
	} else if (field->secure) {
		int32_t spacing = sa_pt(10), dot = sa_pt(7) * SA_SUB / 2;
		int32_t room = (rect.right - rect.left - sa_pt(24)) / spacing;
		int32_t shown = sa_min((int32_t)field->length, room);
		int32_t centre_y = ((rect.top + rect.bottom) * SA_SUB) / 2;

		for (int32_t dot_index = 0; dot_index < shown; dot_index++)
			sa_circle((text_x + dot_index * spacing) * SA_SUB + dot, centre_y, dot, sa_fade(SA_INK, alpha));
		end_x = text_x + shown * spacing;
	} else {
		sa_text(LOGIN_FACE_REGULAR, field->text, text_x, text_y, sa_fade(SA_INK, alpha));
		end_x = text_x + (int32_t)sa_text_width(LOGIN_FACE_REGULAR, field->text) + 1;
	}

	if (focused) sa_record(&g_sa.blink_rect, sa_inset(rect, -sa_max(sa_pt(4), 3)));
	if (focused && ((now - g_sa.caret_at) / SA_CARET_US) % 2U == 0U)
		sa_fill(sa_rect(end_x, text_y, sa_max(sa_pt(2), 1), sa_text_height(LOGIN_FACE_REGULAR)), sa_fade(SA_ACCENT, alpha));
	sa_hit(rect, SA_HIT_FIELD + (int32_t)index);
}

/* How far a control's animation (started at) is, 0..65536. */
static uint32_t
sa_progress(uint64_t at, uint64_t now, uint64_t duration)
{
	return at == 0U ? 65536U : sa_ease_in_out(now - at, duration);
}

/* An iOS-style switch, right-aligned on a row ending at right. */
static void
sa_switch(int32_t right, int32_t centre_y, uint32_t index, uint32_t alpha, uint64_t now)
{
	int32_t width = sa_pt(40), height = sa_pt(24);
	sa_rect_t track = sa_rect(right - width, centre_y - height / 2, width, height);
	uint32_t moved = sa_progress(g_sa.toggle_at[index], now, SA_CONTROL_US);
	uint32_t on = g_sa.toggles[index] ? moved : 65536U - moved;
	int32_t knob_radius = (height / 2 - sa_max(sa_pt(2), 1)) * SA_SUB;
	int32_t from_x = (track.left * SA_SUB) + height * SA_SUB / 2, to_x = (track.right * SA_SUB) - height * SA_SUB / 2;
	int32_t knob_x = from_x + (int32_t)((int64_t)(to_x - from_x) * on / 65536);

	sa_capsule(track, sa_fade(sa_mix_color(SA_SEPARATOR, SA_GREEN, on * 255U / 65536U), alpha));
	sa_circle(knob_x, centre_y * SA_SUB + SA_SUB / 2 + sa_pt(1) * SA_SUB / 2, knob_radius + SA_SUB / 2, sa_fade(0x22000000U, alpha));
	sa_circle(knob_x, centre_y * SA_SUB, knob_radius, sa_fade(SA_WHITE, alpha));
	sa_hit(sa_inset(track, -sa_pt(4)), SA_HIT_TOGGLE + (int32_t)index);
	sa_record(&g_sa.switch_rects[index], sa_inset(track, -sa_pt(4)));
}

/* A grouped list of switch rows (System Settings' look). */
static void
sa_switch_group(sa_rect_t box, const char *const *labels, uint32_t first, uint32_t count, uint32_t alpha, uint64_t now)
{
	int32_t row_height = (box.bottom - box.top) / (int32_t)count;

	sa_squircle(box, (uint32_t)sa_pt(10), sa_fade(SA_FILL, alpha));
	for (uint32_t index = 0U; index < count; index++) {
		int32_t top = box.top + (int32_t)index * row_height;
		int32_t centre = top + row_height / 2;

		if (index > 0U) sa_fill(sa_rect(box.left + sa_pt(14), top, box.right - box.left - sa_pt(28), 1), sa_fade(SA_SEPARATOR, alpha));
		sa_text(LOGIN_FACE_REGULAR, labels[index], box.left + sa_pt(14), centre - sa_text_height(LOGIN_FACE_REGULAR) / 2,
				sa_fade(SA_INK, alpha));
		sa_switch(box.right - sa_pt(12), centre, first + index, alpha, now);
	}
}

/* ---- the clock -------------------------------------------------------------- */

static void
sa_append(char *out, size_t capacity, size_t *used, const char *text)
{
	while (*text != '\0' && *used + 1U < capacity)
		out[(*used)++] = *text++;
	out[*used] = '\0';
}

static void
sa_append_number(char *out, size_t capacity, size_t *used, uint32_t value, uint32_t width)
{
	char digits[12];
	uint32_t count = 0U;

	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U && count < sizeof(digits));
	while (count < width && count < sizeof(digits))
		digits[count++] = '0';
	while (count > 0U && *used + 1U < capacity)
		out[(*used)++] = digits[--count];
	out[*used] = '\0';
}

/* The menu bar clock as the desktop shows it with these options: "Sun Nov 15 2:05:09 PM". */
static void
sa_clock_text(char *out, size_t capacity)
{
	static const char *const weekdays[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
	static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
										   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	int64_t seconds = (int64_t)rtc_unix_time() + g_sa.clock_offset;
	int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
	uint32_t of_day = (uint32_t)(seconds - days * 86400);
	size_t used = 0U;

	/* Civil date from days since 1970 (Howard Hinnant's algorithm). */
	int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
	uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
	uint32_t mp = (5U * doy + 2U) / 153U;
	uint32_t day = doy - (153U * mp + 2U) / 5U + 1U;
	uint32_t month = mp < 10U ? mp + 3U : mp - 9U;

	out[0] = '\0';
	if (g_sa.toggles[2]) {
		sa_append(out, capacity, &used, weekdays[((days % 7) + 7) % 7]);
		sa_append(out, capacity, &used, " ");
	}
	if (g_sa.toggles[3]) {
		sa_append(out, capacity, &used, months[month - 1U]);
		sa_append(out, capacity, &used, " ");
		sa_append_number(out, capacity, &used, day, 1U);
		sa_append(out, capacity, &used, "  ");
	}

	uint32_t hours = of_day / 3600U, minutes = of_day / 60U % 60U;
	bool hours24 = g_sa.toggles[4];

	sa_append_number(out, capacity, &used, hours24 ? hours : (hours % 12U == 0U ? 12U : hours % 12U), hours24 ? 2U : 1U);
	sa_append(out, capacity, &used, ":");
	sa_append_number(out, capacity, &used, minutes, 2U);
	if (g_sa.toggles[5]) {
		sa_append(out, capacity, &used, ":");
		sa_append_number(out, capacity, &used, of_day % 60U, 2U);
	}
	if (!hours24) sa_append(out, capacity, &used, hours < 12U ? " AM" : " PM");
}

/* The time zone timed keeps: OffsetSeconds, if it says. */
static void
sa_load_clock_offset(void)
{
	size_t size = 0U;
	char *text = (char *)login_read_file(SA_TIMED_PLIST, &size, 4096U);
	static const char key[] = "<key>OffsetSeconds</key>";

	g_sa.clock_offset = 0;
	if (text == 0) return;
	for (size_t at = 0U; at + sizeof(key) - 1U < size; at++) {
		if (memcmp(text + at, key, sizeof(key) - 1U) != 0) continue;

		size_t cursor = at + sizeof(key) - 1U;
		while (cursor < size && text[cursor] != '>')
			cursor++;
		cursor++;

		bool negative = cursor < size && text[cursor] == '-';
		int32_t value = 0;
		if (negative) cursor++;
		while (cursor < size && text[cursor] >= '0' && text[cursor] <= '9')
			value = value * 10 + (text[cursor++] - '0');
		g_sa.clock_offset = negative ? -value : value;
		break;
	}
	(void)kfree(text);
}

/* ---- the pages ---------------------------------------------------------------- */

static const char *const sa_titles[SA_PAGES + 1] = {
	"Create your account", "Name your device", "Customize your Dock", "Customize your menu bar", "Choose a wallpaper",
	"Hang tight..." 
};

static const char *const sa_subtitles[SA_PAGES + 1] = {
	"This is the name and password you use to sign in to Grove.",
	"Your device goes by this name in Terminal and on the network.",
	"Pick a size you like. You can change it later in System Settings.",
	"Choose how the clock looks in the menu bar. You can change it later in System Settings.",
	"Pick a picture for your desktop. Golden Gate is yours unless you choose another.",
	"This only takes a moment.",
};

/* The content box of the card: under the title, above the buttons. */
static sa_rect_t
sa_content(sa_rect_t card, int32_t dx)
{
	return (sa_rect_t){card.left + sa_pt(44) + dx, card.top + sa_pt(128), card.right - sa_pt(44) + dx, card.bottom - sa_pt(84)};
}

static void
sa_page_account(sa_rect_t content, uint32_t alpha, uint64_t now)
{
	static const char *const labels[3] = {"Username", "Password", "Verify"};
	static const char *const placeholders[3] = {"lowercase, like alex", "Required", "Type it again"};
	int32_t label_width = sa_pt(110), row = sa_pt(48), height = sa_pt(30);

	for (uint32_t index = 0U; index < 3U; index++) {
		int32_t top = content.top + sa_pt(4) + (int32_t)index * row;

		sa_text(LOGIN_FACE_REGULAR, labels[index],
				content.left + label_width - sa_pt(14) - (int32_t)sa_text_width(LOGIN_FACE_REGULAR, labels[index]),
				top + (height - sa_text_height(LOGIN_FACE_REGULAR)) / 2, sa_fade(SA_SECONDARY, alpha));
		sa_field_draw(sa_rect(content.left + label_width, top, content.right - content.left - label_width, height), index,
					  placeholders[index], alpha, now);
	}

	int32_t note_y = content.top + sa_pt(4) + 3 * row + sa_pt(4);
	if (g_sa.error != 0)
		sa_text(LOGIN_FACE_SMALL, g_sa.error, content.left + label_width, note_y, sa_fade(SA_RED, alpha));
	else
		sa_text(LOGIN_FACE_SMALL, "Your password also unlocks groveOS when it starts, and works with sudo.",
				content.left + label_width, note_y, sa_fade(SA_SECONDARY, alpha));
}

static void
sa_page_device(sa_rect_t content, uint32_t alpha, uint64_t now)
{
	int32_t label_width = sa_pt(110), height = sa_pt(30);
	int32_t top = content.top + sa_pt(4);

	sa_text(LOGIN_FACE_REGULAR, "Device name",
			content.left + label_width - sa_pt(14) - (int32_t)sa_text_width(LOGIN_FACE_REGULAR, "Device name"),
			top + (height - sa_text_height(LOGIN_FACE_REGULAR)) / 2, sa_fade(SA_SECONDARY, alpha));
	sa_field_draw(sa_rect(content.left + label_width, top, content.right - content.left - label_width, height),
				  SA_FIELD_DEVICE, "Required", alpha, now);
	if (g_sa.error != 0) sa_text(LOGIN_FACE_SMALL, g_sa.error, content.left + label_width, top + height + sa_pt(8), sa_fade(SA_RED, alpha));

	/* A Terminal window with the prompt it will show. */
	sa_rect_t window = sa_rect(content.left, top + height + sa_pt(36), content.right - content.left, sa_pt(126));
	char prompt[128];

	sa_squircle(window, (uint32_t)sa_pt(10), sa_fade(0xFF1E1E20U, alpha));
	sa_fill(sa_rect(window.left, window.top + sa_pt(26), window.right - window.left, 1), sa_fade(0xFF2C2C2EU, alpha));
	for (int32_t light = 0; light < 3; light++) {
		static const uint32_t colors[3] = {0xFFFF5F57U, 0xFFFEBC2EU, 0xFF28C840U};
		sa_circle((window.left + sa_pt(14) + light * sa_pt(18)) * SA_SUB, (window.top + sa_pt(13)) * SA_SUB,
				  sa_pt(6) * SA_SUB, sa_fade(colors[light], alpha));
	}
	sa_text_centered(LOGIN_FACE_SMALL, "Terminal", (window.left + window.right) / 2,
					 window.top + (sa_pt(26) - sa_text_height(LOGIN_FACE_SMALL)) / 2, sa_fade(0xFF98989DU, alpha));
	sa_text(LOGIN_FACE_SMALL, "Last login: just now on console", window.left + sa_pt(14), window.top + sa_pt(40),
			sa_fade(0xFF98989DU, alpha));
	(void)sa_join(prompt, sizeof(prompt), g_sa.fields[SA_FIELD_USERNAME].text, "@",
				  g_sa.fields[SA_FIELD_DEVICE].length != 0U ? g_sa.fields[SA_FIELD_DEVICE].text : "...", " ~ %");
	sa_text(LOGIN_FACE_REGULAR, prompt, window.left + sa_pt(14), window.top + sa_pt(64), sa_fade(0xFFF2F2F7U, alpha));
	sa_record(&g_sa.blink_rect, sa_rect(window.left + sa_pt(14) + (int32_t)sa_text_width(LOGIN_FACE_REGULAR, prompt) + sa_pt(6),
										window.top + sa_pt(64), sa_pt(8), sa_text_height(LOGIN_FACE_REGULAR)));
	sa_fill(sa_rect(window.left + sa_pt(14) + (int32_t)sa_text_width(LOGIN_FACE_REGULAR, prompt) + sa_pt(6),
					window.top + sa_pt(64), sa_pt(8), sa_text_height(LOGIN_FACE_REGULAR)),
			sa_fade(((now / SA_CARET_US) % 2U) == 0U ? 0xFFF2F2F7U : 0x00F2F2F7U, alpha));
}

/*
 * Draw a size x size straight-alpha ARGB picture into box, scaled by
 * averaging the source pixels each box pixel covers (premultiplied, so
 * transparent edges do not darken), at alpha.
 */
static void
sa_icon(sa_rect_t box, const uint32_t *source, uint32_t size, uint32_t alpha)
{
	int32_t width = box.right - box.left, height = box.bottom - box.top;
	sa_rect_t area = sa_intersect(box, g_sa.clip);

	if (width <= 0 || height <= 0 || alpha == 0U) return;
	for (int32_t y = area.top; y < area.bottom; y++) {
		uint32_t y0 = (uint32_t)(y - box.top) * size / (uint32_t)height;
		uint32_t y1 = (uint32_t)(y - box.top + 1) * size / (uint32_t)height;
		if (y1 <= y0) y1 = y0 + 1U;

		for (int32_t x = area.left; x < area.right; x++) {
			uint32_t x0 = (uint32_t)(x - box.left) * size / (uint32_t)width;
			uint32_t x1 = (uint32_t)(x - box.left + 1) * size / (uint32_t)width;
			uint32_t sum[4] = {0U, 0U, 0U, 0U}, count = 0U;
			if (x1 <= x0) x1 = x0 + 1U;

			for (uint32_t sy = y0; sy < y1 && sy < size; sy++) {
				for (uint32_t sx = x0; sx < x1 && sx < size; sx++) {
					uint32_t pixel = source[sy * size + sx], a = pixel >> 24U;

					sum[0] += ((pixel >> 16U) & 255U) * a;
					sum[1] += ((pixel >> 8U) & 255U) * a;
					sum[2] += (pixel & 255U) * a;
					sum[3] += a;
					count++;
				}
			}
			if (sum[3] == 0U || count == 0U) continue;

			uint32_t color = ((sum[0] / sum[3]) << 16U) | ((sum[1] / sum[3]) << 8U) | (sum[2] / sum[3]);
			uint32_t coverage = sum[3] / count;
			sa_put(x, y, (alpha << 24U) | color, coverage);
		}
	}
}

/* The Dock preview's tile size now, in pixels: it eases (with a little overshoot) to a new size. */
static int32_t
sa_dock_tile(uint64_t now)
{
	int32_t from = sa_pt((int32_t)sa_dock_sizes[g_sa.dock_size_from] * 64 / 100);
	int32_t to = sa_pt((int32_t)sa_dock_sizes[g_sa.dock_size] * 64 / 100);
	int32_t eased = g_sa.dock_at == 0U ? 65536 : sa_ease_back(now - g_sa.dock_at, SA_DOCK_US);

	return from + (int32_t)((int64_t)(to - from) * eased / 65536);
}

static void
sa_page_dock(sa_rect_t content, uint32_t alpha, uint64_t now)
{
	sa_rect_t preview = sa_rect(content.left, content.top, content.right - content.left, sa_pt(150));

	sa_picture_in(preview, (uint32_t)sa_pt(12), 0, 0U, 0U, alpha, false);

	/*
	 * The Dock as Dock.app lays it out: the kept apps, then (after a
	 * divider) a recent app and the Trash, on a frosted panel. Its own
	 * proportions: icons a sixth of a tile apart, a fifth of a tile of
	 * panel around them. The recent app here is About groveOS.
	 */
	int32_t tile = sa_dock_tile(now);
	int32_t gap = sa_max(tile / 6, 2), pad = sa_max(tile / 5, 3);
	bool recents = g_sa.toggles[0], trash = g_sa.toggles[1];
	int32_t count = (int32_t)SETUP_ICON_APPS + (recents ? 1 : 0) + (trash ? 1 : 0);
	int32_t dividers = (recents || trash) ? 1 : 0;
	int32_t width = count * tile + (count - 1 - dividers) * gap + (dividers ? gap * 2 + 1 : 0) + pad * 2;
	int32_t height = tile + pad * 2;
	sa_rect_t dock = sa_rect((preview.left + preview.right - width) / 2, preview.bottom - height - sa_pt(8), width, height);
	int32_t radius = pad + tile * 225 / 1000;

	sa_squircle(sa_inset(dock, -1), (uint32_t)radius + 1U, sa_fade(0x50FFFFFFU, alpha));
	sa_squircle(dock, (uint32_t)radius, sa_fade(0x60F4F4F6U, alpha));

	int32_t x = dock.left + pad;
	for (uint32_t index = 0U; index < SETUP_ICON_APPS; index++) {
		sa_icon(sa_rect(x, dock.top + pad, tile, tile), &setup_icon_apps[index * SETUP_ICON_SIZE * SETUP_ICON_SIZE],
				SETUP_ICON_SIZE, alpha);
		x += tile + gap;
	}
	if (dividers) {
		x += gap - gap / 2 - 1;
		sa_fill(sa_rect(x, dock.top + pad + tile / 8, 1, tile * 3 / 4), sa_fade(0x70FFFFFFU, alpha));
		x += gap + 1;
	}
	if (recents) {
		/* About groveOS: the logo on a white tile, with the dot of a running app. */
		sa_rect_t box = sa_rect(x, dock.top + pad, tile, tile);
		int32_t inset = tile * 18 / 100;

		sa_squircle(sa_inset(box, tile / 20), (uint32_t)(tile * 22 / 100), sa_fade(0xFFFDFDFDU, alpha));
		sa_icon(sa_inset(box, inset), setup_logo, SETUP_LOGO_SIZE, alpha);
		sa_circle((box.left * 2 + tile) * SA_SUB / 2, (dock.bottom - pad / 2) * SA_SUB, sa_max(tile * SA_SUB / 28, SA_SUB),
				  sa_fade(0xC0000000U, alpha));
		x += tile + gap;
	}
	if (trash) sa_icon(sa_rect(x, dock.top + pad, tile, tile), setup_icon_trash, SETUP_ICON_SIZE, alpha);

	/* The controls. */
	int32_t controls_top = preview.bottom + sa_pt(16);
	sa_text(LOGIN_FACE_REGULAR, "Size", content.left, controls_top + (sa_pt(28) - sa_text_height(LOGIN_FACE_REGULAR)) / 2,
			sa_fade(SA_INK, alpha));
	sa_rect_t segments = sa_rect(content.right - sa_pt(270), controls_top, sa_pt(270), sa_pt(28));
	int32_t segment_width = (segments.right - segments.left) / 3;
	uint32_t moved = sa_progress(g_sa.segment_at, now, SA_CONTROL_US);
	int32_t from_x = segments.left + (int32_t)g_sa.segment_from * segment_width;
	int32_t to_x = segments.left + (int32_t)g_sa.dock_size * segment_width;
	int32_t knob_x = from_x + (int32_t)((int64_t)(to_x - from_x) * moved / 65536);

	sa_record(&g_sa.dock_rect, sa_union(preview, segments));
	sa_squircle(segments, (uint32_t)sa_pt(8), sa_fade(SA_SEPARATOR, alpha));
	sa_squircle(sa_rect(knob_x + 2, segments.top + 2, segment_width - 4, segments.bottom - segments.top - 4),
				(uint32_t)sa_pt(6), sa_fade(SA_WHITE, alpha));
	for (uint32_t index = 0U; index < 3U; index++) {
		sa_rect_t segment = sa_rect(segments.left + (int32_t)index * segment_width, segments.top, segment_width, segments.bottom - segments.top);
		sa_text_centered(index == g_sa.dock_size ? LOGIN_FACE_SEMIBOLD : LOGIN_FACE_REGULAR, sa_dock_size_names[index],
						 (segment.left + segment.right) / 2, segment.top + (segment.bottom - segment.top - sa_text_height(LOGIN_FACE_REGULAR)) / 2,
						 sa_fade(SA_INK, alpha));
		sa_hit(segment, SA_HIT_SEGMENT + (int32_t)index);
	}

	static const char *const labels[2] = {"Show recent apps", "Show the Trash"};
	sa_switch_group(sa_rect(content.left, controls_top + sa_pt(40), content.right - content.left, sa_pt(84)), labels, 0U, 2U,
					alpha, now);
}

/* Rec. 601 brightness of an XRGB pixel, 0..255. */
static uint32_t
sa_luminance(uint32_t pixel)
{
	return (((pixel >> 16U) & 255U) * 77U + ((pixel >> 8U) & 255U) * 150U + (pixel & 255U) * 29U) >> 8U;
}

/*
 * One of the menu bar's glass pills, as the desktop draws them (glass.rs):
 * the picture behind, blurred, frosted toward white (light glass) or smoke
 * (dark glass) by how bright it is, with a 1px rim lit along the top half.
 * Here the picture is the preview's, so `behind` maps a pill pixel to it.
 * True for light glass (dark text).
 */
static bool
sa_glass_pill(sa_rect_t pill, sa_rect_t preview, uint32_t alpha)
{
	const uint32_t *picture = g_sa.backdrop != 0 ? g_sa.backdrop : g_sa.wallpaper;
	int32_t width = preview.right - preview.left;
	uint32_t sum = 0U, samples = 0U;

	/* The preview shows the picture's top, scaled to its width. */
#define SA_BEHIND(px, py)                                                                                             \
	(picture == 0 ? 0x002A3447U                                                                                       \
				  : picture[(uint32_t)sa_min(((py) - preview.top) * g_sa.width / width, g_sa.height - 1) * (uint32_t)g_sa.width + \
							(uint32_t)sa_min(((px) - preview.left) * g_sa.width / width, g_sa.width - 1)])

	for (int32_t y = pill.top; y < pill.bottom; y += 3)
		for (int32_t x = pill.left; x < pill.right; x += 3, samples++)
			/* The backdrop is darkened a little (by 70 of 255): undo that for the choice. */
			sum += sa_min((int32_t)(sa_luminance(SA_BEHIND(x, y)) * (picture == g_sa.backdrop ? 255U : 185U) / 185U), 255);
	bool light = samples != 0U && sum / samples >= 140U;

	uint32_t radius = (uint32_t)(pill.bottom - pill.top) / 2U;
	uint32_t outer_extent = sa_corner_extent(pill, radius);
	const uint8_t *outer = sa_corner(outer_extent);
	sa_rect_t inside = sa_inset(pill, 1);
	uint32_t inner_extent = sa_corner_extent(inside, radius - 1U);
	const uint8_t *inner = sa_corner(inner_extent);
	sa_rect_t area = sa_intersect(pill, g_sa.clip);

	for (int32_t y = area.top; y < area.bottom; y++) {
		bool upper = y < (pill.top + pill.bottom) / 2;
		uint32_t rim_strength = light ? (upper ? 200U : 34U) : (upper ? 96U : 36U);
		uint32_t rim_color = upper || !light ? 0x00FFFFFFU : 0U;

		for (int32_t x = area.left; x < area.right; x++) {
			uint32_t coverage = sa_squircle_coverage(pill, outer_extent, outer, x, y);
			if (coverage == 0U) continue;

			uint32_t behind = SA_BEHIND(x, y);
			if (picture == g_sa.backdrop) behind = sa_blend(behind, 0x00FFFFFFU, 40U);
			uint32_t frosted = light ? sa_blend(behind, 0x00FFFFFFU, 112U) : sa_blend(behind, 0x00141418U, 60U);
			uint32_t in = sa_squircle_coverage(inside, inner_extent, inner, x, y);
			uint32_t rim = coverage > in ? (coverage - in) * rim_strength / 255U : 0U;

			frosted = sa_blend(frosted, rim_color, rim);
			sa_put(x, y, (alpha << 24U) | frosted, coverage);
		}
	}
#undef SA_BEHIND
	return light;
}

/*
 * The menu bar preview: the screen's top strip at its real size, laid out as
 * the desktop's menu bar is (menubar.rs): the logo, the app's name in bold
 * and Window on one glass pill at the left, the clock on its own at the
 * right, 36 points tall, the pills 5 in and their titles 9 either side.
 */
static void
sa_page_menubar(sa_rect_t content, uint32_t alpha, uint64_t now)
{
	sa_rect_t preview = sa_rect(content.left, content.top, content.right - content.left, sa_pt(96));
	uint32_t radius = (uint32_t)sa_pt(12);
	int32_t bar = sa_pt(36), inset = sa_pt(5), padding = sa_pt(9), margin = sa_pt(8);
	int32_t pill_top = preview.top + inset, pill_height = bar - inset * 2;
	char clock[48];

	sa_picture_in(preview, radius, 0, 0U, 0U, alpha, true);
	sa_record(&g_sa.clock_rect, sa_rect(preview.left, preview.top, preview.right - preview.left, bar));

	/* The bar's top shading, inside the preview's corners. */
	uint32_t extent = sa_corner_extent(preview, radius);
	const uint8_t *corner = sa_corner(extent);
	sa_rect_t strip = sa_intersect(sa_rect(preview.left, preview.top, preview.right - preview.left, bar), g_sa.clip);
	for (int32_t y = strip.top; y < strip.bottom; y++) {
		uint32_t shade = 28U * (uint32_t)(bar - (y - preview.top)) / (uint32_t)bar;
		for (int32_t x = strip.left; x < strip.right; x++)
			sa_put(x, y, sa_fade(shade << 24U, alpha), sa_squircle_coverage(preview, extent, corner, x, y));
	}

	/* The left pill: logo, "groveOS" (no app is open yet), Window. */
	int32_t logo = sa_pt(16);
	int32_t name_width = (int32_t)sa_text_width(LOGIN_FACE_SMALL_BOLD, "groveOS");
	int32_t window_width = (int32_t)sa_text_width(LOGIN_FACE_SMALL, "Window");
	int32_t left = preview.left + margin;
	int32_t pill_width = (logo + padding * 2) + (name_width + padding * 2) + (window_width + padding * 2);
	sa_rect_t menus = sa_rect(left, pill_top, pill_width, pill_height);
	bool light = sa_glass_pill(menus, preview, alpha);
	uint32_t ink = light ? 0xFF1D1D1FU : 0xFFFFFFFFU;
	int32_t text_y = pill_top + (pill_height - sa_text_height(LOGIN_FACE_SMALL)) / 2;
	int32_t x = left + padding;

	sa_icon(sa_rect(x, pill_top + (pill_height - logo) / 2, logo, logo), setup_logo, SETUP_LOGO_SIZE, alpha);
	x += logo + padding * 2;
	sa_text(LOGIN_FACE_SMALL_BOLD, "groveOS", x, text_y, sa_fade(ink, alpha));
	x += name_width + padding * 2;
	sa_text(LOGIN_FACE_SMALL, "Window", x, text_y, sa_fade(ink, alpha));

	/* The clock's pill, flush right. */
	sa_clock_text(clock, sizeof(clock));
	int32_t clock_width = (int32_t)sa_text_width(LOGIN_FACE_SMALL, clock) + padding * 2;
	sa_rect_t clock_pill = sa_rect(preview.right - margin - clock_width, pill_top, clock_width, pill_height);
	bool clock_light = sa_glass_pill(clock_pill, preview, alpha);
	sa_text(LOGIN_FACE_SMALL, clock, clock_pill.left + padding, text_y, sa_fade(clock_light ? 0xFF1D1D1FU : 0xFFFFFFFFU, alpha));

	sa_switch_group(sa_rect(content.left, preview.bottom + sa_pt(14), content.right - content.left, sa_pt(160)),
					sa_clock_option_names, 2U, 4U, alpha, now);
}

static void
sa_page_wallpaper(sa_rect_t content, uint32_t alpha)
{
	int32_t width = (int32_t)sa_thumbnail_width(), height = (int32_t)sa_thumbnail_height();
	int32_t gap = sa_pt(16), label = sa_pt(24);
	int32_t grid_width = 3 * width + 2 * gap;
	int32_t left = (content.left + content.right - grid_width) / 2;
	uint32_t radius = (uint32_t)sa_pt(10);

	if (g_sa.picture_count == 0U) {
		sa_picture_in(sa_rect(left + width + gap, content.top + sa_pt(8), width, height), radius, 0, 0U, 0U, alpha, false);
		sa_squircle_ring(sa_rect(left + width + gap, content.top + sa_pt(8), width, height), radius, sa_max(sa_pt(3), 2),
						 sa_fade(SA_ACCENT, alpha));
		sa_text_centered(LOGIN_FACE_SMALL, "Golden Gate", left + width + gap + width / 2, content.top + sa_pt(8) + height + sa_pt(6),
						 sa_fade(SA_INK, alpha));
		return;
	}

	for (uint32_t index = 0U; index < g_sa.picture_count; index++) {
		sa_picture_t *picture = &g_sa.pictures[index];
		int32_t column = (int32_t)(index % 3U), row = (int32_t)(index / 3U);
		sa_rect_t box = sa_rect(left + column * (width + gap), content.top + sa_pt(6) + row * (height + label + gap / 2), width, height);
		char name[64];

		if (picture->thumbnail != 0)
			sa_picture_in(box, radius, picture->thumbnail, (uint32_t)width, (uint32_t)height, alpha, false);
		else
			sa_squircle(box, radius, sa_fade(SA_SEPARATOR, alpha));
		if ((int32_t)index == g_sa.picture) sa_squircle_ring(box, radius, sa_max(sa_pt(3), 2), sa_fade(SA_ACCENT, alpha));
		sa_pretty_name(picture->name, name, sizeof(name));
		sa_text_centered(LOGIN_FACE_SMALL, name, (box.left + box.right) / 2, box.bottom + sa_pt(6),
						 sa_fade((int32_t)index == g_sa.picture ? SA_INK : SA_SECONDARY, alpha));
		sa_hit(sa_inset(box, -sa_pt(4)), SA_HIT_PICTURE + (int32_t)index);
	}
}

/* A spinner: twelve spokes, the brightest at the time's angle. */
static void
sa_spinner(int32_t cx, int32_t cy, int32_t radius, uint32_t alpha, uint64_t now)
{
	/* Spoke directions at 30-degree steps, as (cos, sin) * 1000. */
	static const int32_t spokes[12][2] = {{1000, 0},	 {866, 500},	{500, 866},	  {0, 1000},	{-500, 866}, {-866, 500},
										  {-1000, 0}, {-866, -500}, {-500, -866}, {0, -1000}, {500, -866}, {866, -500}};
	uint32_t lead = (uint32_t)(now / 83000U % 12U);

	for (uint32_t spoke = 0U; spoke < 12U; spoke++) {
		uint32_t age = (lead + 12U - spoke) % 12U;
		uint32_t shade = 255U - age * 18U;
		const int32_t piece[1][4] = {{cx * SA_SUB + spokes[spoke][0] * radius * SA_SUB / 2000,
									  cy * SA_SUB + spokes[spoke][1] * radius * SA_SUB / 2000,
									  cx * SA_SUB + spokes[spoke][0] * radius * SA_SUB / 1000,
									  cy * SA_SUB + spokes[spoke][1] * radius * SA_SUB / 1000}};
		sa_stroke(piece, 1U, sa_max(radius * SA_SUB / 12, SA_SUB / 2), sa_fade((shade * 140U / 255U) << 24U, alpha));
	}
}

/* One page's title and content at alpha, shifted dx across. */
static void
sa_page(sa_page_t page, sa_rect_t card, uint32_t alpha, int32_t dx, uint64_t now)
{
	int32_t centre = (card.left + card.right) / 2 + dx;
	sa_rect_t content = sa_content(card, dx);

	if (alpha == 0U) return;
	sa_text_centered(LOGIN_FACE_TITLE, sa_titles[page], centre, card.top + sa_pt(42), sa_fade(0xF0000000U, alpha));
	sa_text_centered(LOGIN_FACE_REGULAR, sa_subtitles[page], centre, card.top + sa_pt(42) + sa_text_height(LOGIN_FACE_TITLE) + sa_pt(10),
					 sa_fade(SA_SECONDARY, alpha));

	switch (page) {
	case SA_PAGE_ACCOUNT:
		sa_page_account(content, alpha, now);
		break;
	case SA_PAGE_DEVICE:
		sa_page_device(content, alpha, now);
		break;
	case SA_PAGE_DOCK:
		sa_page_dock(content, alpha, now);
		break;
	case SA_PAGE_MENUBAR:
		sa_page_menubar(content, alpha, now);
		break;
	case SA_PAGE_WALLPAPER:
		sa_page_wallpaper(content, alpha);
		break;
	default:
		sa_spinner((content.left + content.right) / 2, content.top + sa_pt(70), sa_pt(18), alpha, now);
		break;
	}
}

/* Whether the card is at rest, as its cached base shows it. */
static bool
sa_card_at_rest(void)
{
	return g_sa.card_alpha == 255U && g_sa.card_rise == 0 && g_sa.hello_alpha == 0U && g_sa.mix >= 255U;
}

/* Keep what is on screen of the card's area as its base (before any contents are drawn). */
static void
sa_card_keep_base(sa_rect_t area)
{
	int32_t width = area.right - area.left, height = area.bottom - area.top;

	if (width <= 0 || height <= 0) return;
	if (g_sa.card_base == 0) g_sa.card_base = kmalloc((size_t)width * (size_t)height * sizeof(uint32_t));
	if (g_sa.card_base == 0) return;
	for (int32_t y = 0; y < height; y++)
		memcpy(&g_sa.card_base[(size_t)y * (size_t)width],
			   &g_sa.display->framebuffer[(uint64_t)(area.top + y) * g_sa.display->stride + (uint32_t)area.left],
			   (size_t)width * sizeof(uint32_t));
	g_sa.card_base_area = area;
	g_sa.card_base_valid = true;
}

/* Put the cached base back under rect (inside its area). */
static void
sa_card_restore_base(sa_rect_t rect)
{
	sa_rect_t area = g_sa.card_base_area;
	int32_t width = area.right - area.left;

	rect = sa_intersect(rect, area);
	for (int32_t y = rect.top; y < rect.bottom; y++)
		memcpy(&g_sa.display->framebuffer[(uint64_t)y * g_sa.display->stride + (uint32_t)rect.left],
			   &g_sa.card_base[(size_t)(y - area.top) * (size_t)width + (size_t)(rect.left - area.left)],
			   (size_t)(rect.right - rect.left) * sizeof(uint32_t));
}

/*
 * Paint the card as it is now: the whole screen, its area (whole_screen
 * false, part empty), or just `part` of it. At rest, a frame starts from the
 * cached base; everything after is clipped to what is being repainted.
 */
static void
sa_card_paint(uint64_t now, bool whole_screen, sa_rect_t part)
{
	sa_rect_t card = sa_card();
	sa_rect_t area = whole_screen ? sa_screen() : sa_empty(part) ? sa_card_area() : sa_intersect(part, sa_card_area());
	uint32_t alpha = g_sa.card_alpha;
	bool rest = sa_card_at_rest();
	bool from_base = rest && g_sa.card_base_valid && !whole_screen &&
					 area.left >= g_sa.card_base_area.left && area.top >= g_sa.card_base_area.top &&
					 area.right <= g_sa.card_base_area.right && area.bottom <= g_sa.card_base_area.bottom;
	bool whole_card = !whole_screen && sa_empty(part);

	sa_set_clip(area);
	login_cursor_hide(g_sa.display);
	g_sa.hit_count = 0U;
	if (whole_card) {
		/* A whole frame of the card records afresh where its moving parts are. */
		g_sa.blink_rect = g_sa.dock_rect = g_sa.clock_rect = (sa_rect_t){0, 0, 0, 0};
		for (uint32_t index = 0U; index < 6U; index++) g_sa.switch_rects[index] = (sa_rect_t){0, 0, 0, 0};
	}

	if (from_base) {
		sa_card_restore_base(area);
		goto contents;
	}
	sa_restore(g_sa.clip);

	if (g_sa.hello_alpha != 0U) {
		sa_hello_layout_t layout = sa_hello_layout();

		g_sa.hits_live = false;
		sa_hello_word(&layout, LOGIN_HELLO_LENGTH, g_sa.hello_alpha);
		sa_hello_prompt(&layout, g_sa.hello_alpha, false);
	}

	if (alpha != 0U) {
		sa_shadow(card, sa_pt(22), sa_pt(40), sa_pt(16), alpha * 70U / 255U);
		sa_squircle(sa_inset(card, -1), (uint32_t)sa_pt(22) + 1U, sa_fade(0x30000000U, alpha));
		sa_squircle(card, (uint32_t)sa_pt(22), sa_fade(SA_WHITE, alpha));
		if (rest && whole_card) sa_card_keep_base(sa_card_area());
	}

contents:
	if (alpha != 0U) {

		/*
		 * Changing pages: the old one slides out and fades over the first
		 * 45%, the new one slides in and fades up over the last 70%, each on
		 * its own ease, so they barely overlap.
		 */
		uint64_t since = g_sa.from_page == g_sa.page ? SA_PAGE_US : now - g_sa.page_at;
		uint32_t leaving = sa_ease_in_out(since, SA_PAGE_US * 45U / 100U);
		uint32_t arriving = since < SA_PAGE_US * 30U / 100U ? 0U : sa_ease_in_out(since - SA_PAGE_US * 30U / 100U, SA_PAGE_US * 70U / 100U);
		bool settled = arriving >= 65536U && alpha == 255U;
		int32_t slide = sa_pt(44) * (g_sa.page >= g_sa.from_page ? 1 : -1);

		g_sa.hits_live = settled;
		if (!settled && g_sa.from_page != g_sa.page && leaving < 65536U) {
			g_sa.hits_live = false;
			sa_page(g_sa.from_page, card, alpha * (65536U - leaving) / 65536U, -(int32_t)((int64_t)slide * leaving / 65536), now);
		}
		sa_page(g_sa.page, card, alpha * arriving / 65536U, (int32_t)((int64_t)slide * (65536 - (int64_t)arriving) / 65536), now);

		/* The buttons and the step dots stay put while the pages change. */
		if (g_sa.page < SA_PAGES) {
			int32_t button_y = card.bottom - sa_pt(62), button_height = sa_pt(34);
			bool last = g_sa.page == SA_PAGE_WALLPAPER;

			if (g_sa.page > SA_PAGE_ACCOUNT)
				sa_button(sa_rect(card.left + sa_pt(44), button_y, sa_pt(100), button_height), "Back", false, true, alpha, SA_HIT_BACK);
			sa_button(sa_rect(card.right - sa_pt(44) - sa_pt(128), button_y, sa_pt(128), button_height),
					  last ? "Set Up" : "Continue", true, true, alpha, SA_HIT_CONTINUE);

			int32_t dot_y = (button_y + button_height / 2) * SA_SUB, spacing = sa_pt(16);
			int32_t first_x = (card.left + card.right) / 2 - spacing * 2;
			for (int32_t index = 0; index < SA_PAGES; index++)
				sa_circle((first_x + index * spacing) * SA_SUB, dot_y, sa_pt(7) * SA_SUB / 2,
						  sa_fade(index == (int32_t)g_sa.page ? SA_INK : 0xFFD1D1D6U, alpha));
		}
		g_sa.hits_live = false;
	}
	sa_present(area);
}

static void
sa_card_frame(uint64_t now, bool whole_screen)
{
	sa_card_paint(now, whole_screen, (sa_rect_t){0, 0, 0, 0});
}

/* ---- typing and validating ---------------------------------------------------- */

static bool
sa_name_character(char *character, bool hostname)
{
	char c = *character;

	if (c >= 'A' && c <= 'Z') {
		if (!hostname) *character = (char)(c - 'A' + 'a');
		return true;
	}
	if (c == ' ' && hostname) {
		*character = '-';
		return true;
	}
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || (c == '_' && !hostname) ||
		   (c == '.' && hostname);
}

static void
sa_type(uint32_t index, char character)
{
	sa_field_t *field = &g_sa.fields[index];

	if (index == SA_FIELD_USERNAME || index == SA_FIELD_DEVICE) {
		if (!sa_name_character(&character, index == SA_FIELD_DEVICE)) return;
	} else if (character < 32 || character > 126) {
		return;
	}
	if (field->length >= field->capacity) return;
	field->text[field->length++] = character;
	field->text[field->length] = '\0';
	g_sa.error = 0;
}

static void
sa_erase(uint32_t index)
{
	sa_field_t *field = &g_sa.fields[index];

	if (field->length == 0U) return;
	field->text[--field->length] = '\0';
}

static bool
sa_reserved_name(const char *name)
{
	static const char *const reserved[] = {"root", "nobody", "wheel", "tty", "staff", "daemon", "bin", "sys", "admin"};

	for (uint32_t index = 0U; index < sizeof(reserved) / sizeof(reserved[0]); index++)
		if (strcmp(name, reserved[index]) == 0) return true;
	return false;
}

/* Whether the page's answers can go through; if not, the error says why. */
static bool
sa_validate(void)
{
	sa_field_t *username = &g_sa.fields[SA_FIELD_USERNAME];
	sa_field_t *password = &g_sa.fields[SA_FIELD_PASSWORD];
	sa_field_t *verify = &g_sa.fields[SA_FIELD_VERIFY];

	switch (g_sa.page) {
	case SA_PAGE_ACCOUNT:
		if (username->length == 0U) {
			g_sa.error = "Choose a username.";
			g_sa.focus = SA_FIELD_USERNAME;
		} else if (username->text[0] < 'a' || username->text[0] > 'z') {
			g_sa.error = "Usernames start with a letter.";
			g_sa.focus = SA_FIELD_USERNAME;
		} else if (sa_reserved_name(username->text)) {
			g_sa.error = "That name belongs to the system. Choose another.";
			g_sa.focus = SA_FIELD_USERNAME;
		} else if (password->length < 4U) {
			g_sa.error = "Use at least four characters for your password.";
			g_sa.focus = SA_FIELD_PASSWORD;
		} else if (verify->length != password->length || memcmp(verify->text, password->text, password->length) != 0) {
			g_sa.error = "The passwords do not match.";
			sa_wipe(verify->text, sizeof(verify->text));
			verify->length = 0U;
			g_sa.focus = SA_FIELD_VERIFY;
		} else {
			return true;
		}
		return false;
	case SA_PAGE_DEVICE:
		if (g_sa.fields[SA_FIELD_DEVICE].length == 0U) {
			g_sa.error = "Give your device a name.";
			return false;
		}
		return true;
	default:
		return true;
	}
}

static void
sa_go(sa_page_t page, uint64_t now)
{
	if (page == g_sa.page) return;
	g_sa.from_page = g_sa.page;
	g_sa.page = page;
	g_sa.page_at = now;
	g_sa.error = 0;
	g_sa.focus = 0U;
	g_sa.caret_at = now;

	if (page == SA_PAGE_DEVICE && g_sa.fields[SA_FIELD_DEVICE].length == 0U) {
		char name[48];
		(void)sa_join(name, sizeof(name), g_sa.fields[SA_FIELD_USERNAME].text, "-groveos", 0, 0);
		for (const char *character = name; *character != '\0'; character++)
			sa_type(SA_FIELD_DEVICE, *character);
	}
	if (page == SA_PAGE_WALLPAPER) sa_load_pictures();
}

/* ---- applying ----------------------------------------------------------------- */

static void
sa_hmac(const uint8_t *key, size_t key_length, const uint8_t *first, size_t first_length, const uint8_t *second,
		size_t second_length, uint8_t out[SHA256_DIGEST_SIZE])
{
	uint8_t block[SHA256_BLOCK_SIZE] = {0};
	uint8_t inner[SHA256_DIGEST_SIZE];
	sha256_context_t context;

	if (key_length > SHA256_BLOCK_SIZE) sha256(key, key_length, block);
	else memcpy(block, key, key_length);

	for (uint32_t index = 0U; index < SHA256_BLOCK_SIZE; index++) block[index] ^= 0x36U;
	sha256_init(&context);
	sha256_update(&context, block, SHA256_BLOCK_SIZE);
	sha256_update(&context, first, first_length);
	if (second_length != 0U) sha256_update(&context, second, second_length);
	sha256_final(&context, inner);

	for (uint32_t index = 0U; index < SHA256_BLOCK_SIZE; index++) block[index] ^= 0x36U ^ 0x5CU;
	sha256_init(&context);
	sha256_update(&context, block, SHA256_BLOCK_SIZE);
	sha256_update(&context, inner, SHA256_DIGEST_SIZE);
	sha256_final(&context, out);
	sa_wipe(block, sizeof(block));
	sa_wipe(inner, sizeof(inner));
	sa_wipe(&context, sizeof(context));
}

/* PBKDF2-HMAC-SHA256, one 32-byte block: the /etc/shadow hash (frameworks/lib/auth.c). */
static void
sa_pbkdf2(const char *password, size_t length, const uint8_t *salt, uint32_t iterations, uint8_t out[SHA256_DIGEST_SIZE])
{
	static const uint8_t first_block[4] = {0U, 0U, 0U, 1U};
	uint8_t u[SHA256_DIGEST_SIZE];

	sa_hmac((const uint8_t *)password, length, salt, SA_SALT_BYTES, first_block, sizeof(first_block), u);
	memcpy(out, u, sizeof(u));
	for (uint32_t iteration = 1U; iteration < iterations; iteration++) {
		sa_hmac((const uint8_t *)password, length, u, sizeof(u), 0, 0U, u);
		for (uint32_t index = 0U; index < SHA256_DIGEST_SIZE; index++) out[index] ^= u[index];
	}
	sa_wipe(u, sizeof(u));
}

static void
sa_hex(const uint8_t *bytes, size_t length, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t index = 0U; index < length; index++) {
		out[index * 2U] = digits[bytes[index] >> 4];
		out[index * 2U + 1U] = digits[bytes[index] & 15U];
	}
	out[length * 2U] = '\0';
}

/* "pbkdf2_sha256$4096$<salt>$<hash>", as nxu_pwhash_make writes it. */
static bool
sa_password_hash(char *out, size_t capacity)
{
	uint8_t salt[SA_SALT_BYTES], hash[SHA256_DIGEST_SIZE];
	char salt_hex[SA_SALT_BYTES * 2U + 1U], hash_hex[SHA256_DIGEST_SIZE * 2U + 1U];
	size_t used = 0U;

	if (tep_random(salt, sizeof(salt)) != TEP_OK) {
		/* tepOS answered for the password a moment ago; still, never a fixed salt. */
		uint64_t now = timer_get_microseconds();
		uint8_t digest[SHA256_DIGEST_SIZE];

		sha256(&now, sizeof(now), digest);
		memcpy(salt, digest, sizeof(salt));
	}
	sa_pbkdf2(g_sa.fields[SA_FIELD_PASSWORD].text, g_sa.fields[SA_FIELD_PASSWORD].length, salt, SA_PBKDF2_ITERATIONS, hash);
	sa_hex(salt, sizeof(salt), salt_hex);
	sa_hex(hash, sizeof(hash), hash_hex);
	out[0] = '\0';
	sa_append(out, capacity, &used, "pbkdf2_sha256$");
	sa_append_number(out, capacity, &used, SA_PBKDF2_ITERATIONS, 1U);
	sa_append(out, capacity, &used, "$");
	sa_append(out, capacity, &used, salt_hex);
	sa_append(out, capacity, &used, "$");
	sa_append(out, capacity, &used, hash_hex);
	sa_wipe(hash, sizeof(hash));
	return used + 1U < capacity;
}

static bool
sa_write_file(const char *path, const char *data, size_t length)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;
	size_t done = 0U;

	if (vfs_open(filedesc, path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) != VFS_STATUS_OK) {
		kprintf("could not open %s for writing\n", path);
		return false;
	}
	while (done < length) {
		uint64_t written = 0U;

		if (vfs_write(filedesc, descriptor, data + done, length - done, &written) != VFS_STATUS_OK || written == 0U) break;
		done += (size_t)written;
	}
	(void)vfs_close(filedesc, descriptor);
	if (done != length) kprintf("could not write all of %s\n", path);
	return done == length;
}

/* The nth ':' field of a line, as a pointer and length. */
static bool
sa_line_field(const char *line, size_t length, uint32_t wanted, const char **field, size_t *field_length)
{
	uint32_t index = 0U;
	size_t start = 0U;

	for (size_t at = 0U; at <= length; at++) {
		if (at == length || line[at] == ':') {
			if (index == wanted) {
				*field = line + start;
				*field_length = at - start;
				return true;
			}
			index++;
			start = at + 1U;
		}
	}
	return false;
}

static bool
sa_field_is(const char *field, size_t length, const char *text)
{
	return strlen(text) == length && memcmp(field, text, length) == 0;
}

typedef struct {
	char *data;
	size_t used, capacity;
} sa_buffer_t;

static void
sa_put_bytes(sa_buffer_t *buffer, const char *bytes, size_t length)
{
	if (buffer->used + length >= buffer->capacity) return;
	memcpy(buffer->data + buffer->used, bytes, length);
	buffer->used += length;
}

static void
sa_put_text(sa_buffer_t *buffer, const char *text)
{
	sa_put_bytes(buffer, text, strlen(text));
}

/*
 * The account (uid 1000, "user" on a fresh disk) renamed to the chosen
 * username, with the chosen password, in /etc/passwd, /etc/group and
 * /etc/shadow. Its home stays where it is.
 */
static void
sa_write_accounts(const char *hash)
{
	static const char *const paths[3] = {SA_PASSWD, SA_GROUP, SA_SHADOW};
	const char *name = g_sa.fields[SA_FIELD_USERNAME].text;
	char old_name[40] = "user";
	char full_name[40];

	/* "alex" is "Alex" in the full-name field. */
	(void)sa_join(full_name, sizeof(full_name), name, 0, 0, 0);
	if (full_name[0] >= 'a' && full_name[0] <= 'z') full_name[0] = (char)(full_name[0] - 'a' + 'A');

	for (uint32_t file = 0U; file < 3U; file++) {
		size_t size = 0U;
		char *text = (char *)login_read_file(paths[file], &size, 65536U);
		sa_buffer_t out = {kmalloc(size + 512U), 0U, size + 512U};

		if (text == 0 || out.data == 0) {
			kprintf("could not read %s: the account keeps its old name there\n", paths[file]);
			if (text != 0) (void)kfree(text);
			if (out.data != 0) (void)kfree(out.data);
			continue;
		}

		for (size_t start = 0U; start < size;) {
			size_t end = start;
			while (end < size && text[end] != '\n')
				end++;

			const char *line = text + start, *first, *third;
			size_t length = end - start, first_length = 0U, third_length = 0U;
			bool has_first = sa_line_field(line, length, 0U, &first, &first_length);
			bool has_third = sa_line_field(line, length, 2U, &third, &third_length);
			bool ours = has_first && (sa_field_is(first, first_length, old_name) ||
									  (file != 2U && has_third && sa_field_is(third, third_length, SA_ACCOUNT_UID)));

			if (file == 0U && ours) {
				/* passwd: name:x:uid:gid:gecos:home:shell */
				const char *home, *shell;
				size_t home_length, shell_length;

				if (first_length < sizeof(old_name)) {
					memcpy(old_name, first, first_length);
					old_name[first_length] = '\0';
				}
				sa_put_text(&out, name);
				sa_put_text(&out, ":x:" SA_ACCOUNT_UID ":" SA_ACCOUNT_UID ":");
				sa_put_text(&out, full_name);
				sa_put_text(&out, ":");
				if (sa_line_field(line, length, 5U, &home, &home_length)) sa_put_bytes(&out, home, home_length);
				sa_put_text(&out, ":");
				if (sa_line_field(line, length, 6U, &shell, &shell_length)) sa_put_bytes(&out, shell, shell_length);
			} else if (file == 1U && has_first) {
				/* group: name:x:gid:members, the account renamed among the members too. */
				const char *members;
				size_t members_length = 0U;

				if (ours) sa_put_text(&out, name);
				else sa_put_bytes(&out, first, first_length);
				sa_put_bytes(&out, first + first_length, has_third ? (size_t)(third + third_length - (first + first_length)) : 0U);
				sa_put_text(&out, ":");
				if (has_third && sa_line_field(line, length, 3U, &members, &members_length)) {
					for (size_t at = 0U; at <= members_length;) {
						size_t stop = at;
						while (stop < members_length && members[stop] != ',')
							stop++;
						if (at > 0U) sa_put_text(&out, ",");
						if (sa_field_is(members + at, stop - at, old_name)) sa_put_text(&out, name);
						else sa_put_bytes(&out, members + at, stop - at);
						at = stop + 1U;
					}
				}
			} else if (file == 2U && ours) {
				sa_put_text(&out, name);
				sa_put_text(&out, ":");
				sa_put_text(&out, hash);
			} else {
				sa_put_bytes(&out, line, length);
			}
			if (end < size) sa_put_text(&out, "\n");
			start = end + 1U;
		}

		if (sa_write_file(paths[file], out.data, out.used)) kprintf("%s: the account is now %s\n", paths[file], name);
		(void)kfree(text);
		sa_wipe(out.data, out.capacity);
		(void)kfree(out.data);
	}
}

static const char sa_plist_head[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<!DOCTYPE plist PUBLIC \"-//NXU//DTD PLIST 1.0//EN\" \"http://www.nxu.local/DTDs/PropertyList-1.0.dtd\">\n"
	"<plist version=\"1.0\">\n<dict>\n";
static const char sa_plist_tail[] = "</dict>\n</plist>\n";

static void
sa_plist_bool(sa_buffer_t *out, const char *key, bool value)
{
	sa_put_text(out, "\t<key>");
	sa_put_text(out, key);
	sa_put_text(out, value ? "</key>\n\t<true/>\n" : "</key>\n\t<false/>\n");
}

static void
sa_plist_string(sa_buffer_t *out, const char *key, const char *value)
{
	sa_put_text(out, "\t<key>");
	sa_put_text(out, key);
	sa_put_text(out, "</key>\n\t<string>");
	sa_put_text(out, value);
	sa_put_text(out, "</string>\n");
}

static void
sa_write_preferences(void)
{
	char text[1024];
	sa_buffer_t out;
	char number[12];
	size_t used = 0U;

	/* The Dock. */
	out = (sa_buffer_t){text, 0U, sizeof(text)};
	sa_put_text(&out, sa_plist_head);
	sa_put_text(&out, "\t<key>tilesize</key>\n\t<integer>");
	sa_append_number(number, sizeof(number), &used, sa_dock_sizes[g_sa.dock_size], 1U);
	sa_put_text(&out, number);
	sa_put_text(&out, "</integer>\n");
	sa_plist_bool(&out, "show-recents", g_sa.toggles[0]);
	sa_plist_bool(&out, "show-trash", g_sa.toggles[1]);
	sa_put_text(&out, sa_plist_tail);
	(void)sa_write_file(SA_DOCK_PLIST, out.data, out.used);

	/* The menu bar clock. */
	out = (sa_buffer_t){text, 0U, sizeof(text)};
	sa_put_text(&out, sa_plist_head);
	sa_plist_bool(&out, "ShowDayOfWeek", g_sa.toggles[2]);
	sa_plist_bool(&out, "ShowDate", g_sa.toggles[3]);
	sa_plist_bool(&out, "Show24Hour", g_sa.toggles[4]);
	sa_plist_bool(&out, "ShowSeconds", g_sa.toggles[5]);
	sa_put_text(&out, sa_plist_tail);
	(void)sa_write_file(SA_CLOCK_PLIST, out.data, out.used);

	/* The wallpaper (GoldenGate unless another was chosen). */
	char picture[200];
	if (g_sa.picture >= 0 && (uint32_t)g_sa.picture < g_sa.picture_count)
		(void)sa_join(picture, sizeof(picture), g_sa.pictures[g_sa.picture].directory, "/", g_sa.pictures[g_sa.picture].name, ".png");
	else
		(void)sa_join(picture, sizeof(picture), SA_PHOTOS_DIR "/", SA_DEFAULT_PICTURE, ".png", 0);
	out = (sa_buffer_t){text, 0U, sizeof(text)};
	sa_put_text(&out, sa_plist_head);
	sa_plist_string(&out, "Picture", picture);
	sa_plist_string(&out, "Placement", "Fill Screen");
	sa_put_text(&out, sa_plist_tail);
	(void)sa_write_file(SA_WALLPAPER_PLIST, out.data, out.used);

	/* The desktop's welcome, once. */
	out = (sa_buffer_t){text, 0U, sizeof(text)};
	sa_put_text(&out, sa_plist_head);
	sa_plist_string(&out, "Welcome", g_sa.fields[SA_FIELD_USERNAME].text);
	sa_put_text(&out, sa_plist_tail);
	(void)sa_write_file(SA_WELCOME_PLIST, out.data, out.used);
}

/*
 * Make it so. The password goes to tepOS first: that is what unlocks, and
 * if it fails nothing else is written. The rest is best effort, logged.
 */
static bool
sa_apply(void)
{
	sa_field_t *password = &g_sa.fields[SA_FIELD_PASSWORD];
	uint32_t wait_seconds = 0U;
	tep_result_t result = tep_auth_set(0, 0, (const uint8_t *)password->text, password->length, &wait_seconds);
	char hash[128];

	if (result != TEP_OK) {
		kprintf("tepOS did not take the password: %s\n", tep_result_name(result));
		g_sa.error = "The Trusted Enclave could not save your password. Try again.";
		return false;
	}
	kprintf("password set in tepOS\n");

	if (sa_password_hash(hash, sizeof(hash))) sa_write_accounts(hash);
	sa_wipe(hash, sizeof(hash));

	char line[48];
	(void)sa_join(line, sizeof(line), g_sa.fields[SA_FIELD_DEVICE].text, "\n", 0, 0);
	if (sa_write_file(SA_HOSTNAME, line, strlen(line))) kprintf("this device is %s", line);

	sa_write_preferences();
	(void)vfs_sync_all();
	kprintf("set up for %s\n", g_sa.fields[SA_FIELD_USERNAME].text);
	return true;
}

/* ---- input -------------------------------------------------------------------- */

static void
sa_toggle(uint32_t index, uint64_t now)
{
	g_sa.toggles[index] = !g_sa.toggles[index];
	g_sa.toggle_at[index] = now;
}

static void
sa_continue(uint64_t now)
{
	if (!sa_validate()) return;
	if (g_sa.page == SA_PAGE_WALLPAPER) {
		sa_go(SA_PAGE_APPLY, now);
		return;
	}
	sa_go((sa_page_t)(g_sa.page + 1), now);
}

static void
sa_card_click(int32_t id, uint64_t now)
{
	if (id == SA_HIT_CONTINUE) {
		sa_continue(now);
	} else if (id == SA_HIT_BACK && g_sa.page > SA_PAGE_ACCOUNT) {
		sa_go((sa_page_t)(g_sa.page - 1), now);
	} else if (id >= SA_HIT_FIELD) {
		g_sa.focus = (uint32_t)(id - SA_HIT_FIELD);
		g_sa.caret_at = now;
	} else if (id >= SA_HIT_PICTURE) {
		sa_choose_picture(id - SA_HIT_PICTURE);
	} else if (id >= SA_HIT_TOGGLE) {
		sa_toggle((uint32_t)(id - SA_HIT_TOGGLE), now);
	} else if (id >= SA_HIT_SEGMENT && (uint32_t)(id - SA_HIT_SEGMENT) != g_sa.dock_size) {
		g_sa.segment_from = g_sa.dock_size;
		g_sa.dock_size_from = g_sa.dock_size;
		g_sa.dock_size = (uint32_t)(id - SA_HIT_SEGMENT);
		g_sa.segment_at = now;
		g_sa.dock_at = now;
	}
}

static void
sa_card_key(const keyboard_key_t *key, uint64_t now)
{
	uint32_t field = g_sa.page == SA_PAGE_ACCOUNT ? g_sa.focus : SA_FIELD_DEVICE;
	bool typing = g_sa.page == SA_PAGE_ACCOUNT || g_sa.page == SA_PAGE_DEVICE;

	g_sa.caret_at = now;
	if (key->code == 28) {
		/* Return: the next field, or on. */
		if (g_sa.page == SA_PAGE_ACCOUNT && g_sa.focus < SA_FIELD_VERIFY && g_sa.fields[g_sa.focus].length != 0U)
			g_sa.focus++;
		else
			sa_continue(now);
	} else if (key->code == 15) {
		if (g_sa.page == SA_PAGE_ACCOUNT) g_sa.focus = (g_sa.focus + 1U) % 3U;
	} else if (key->code == 1) {
		if (g_sa.page > SA_PAGE_ACCOUNT) sa_go((sa_page_t)(g_sa.page - 1), now);
	} else if (key->code == 14) {
		if (typing) sa_erase(field);
	} else if (typing && key->character >= 32 && key->character < 127) {
		sa_type(field, (char)key->character);
	}
}

/* ---- the run ------------------------------------------------------------------- */

static void
sa_enter(sa_phase_t phase, uint64_t now)
{
	g_sa.phase = phase;
	g_sa.phase_at = now;
	g_sa.written = 0U;
	g_sa.shown_prompt = 0U;
}

/* One frame of whatever is on screen. True when the run is over. */
static bool
sa_frame(uint64_t now, bool *dirty)
{
	sa_hello_layout_t layout = sa_hello_layout();
	uint64_t elapsed = now - g_sa.phase_at;

	switch (g_sa.phase) {
	case SA_HELLO:
	case SA_DONE: {
		bool finish = g_sa.phase == SA_DONE;
		uint64_t pen = elapsed > SA_WRITE_DELAY_US ? elapsed - SA_WRITE_DELAY_US : 0U;
		uint32_t reveal = sa_hello_reveal(sa_smoothstep(pen, SA_WRITE_US));
		uint32_t prompt = elapsed < SA_PROMPT_AT_US ? 0U : sa_ease_out(elapsed - SA_PROMPT_AT_US, SA_PROMPT_FADE_US);

		if (*dirty) {
			sa_set_clip(sa_screen());
			login_cursor_hide(g_sa.display);
			sa_restore(g_sa.clip);
			sa_present(g_sa.clip);
			g_sa.written = 0U;
			g_sa.shown_prompt = 0U;
			*dirty = false;
		}
		if (reveal != g_sa.written) {
			sa_hello_frame(&layout, sa_hello_written(&layout, g_sa.written, reveal), reveal, 255U, 0U, finish);
			g_sa.written = reveal;
		}
		if (prompt != g_sa.shown_prompt) {
			sa_rect_t below = {0, layout.hello_y + (int32_t)layout.hello_height, g_sa.width, layout.band.bottom};
			sa_hello_frame(&layout, below, reveal, 255U, prompt, finish);
			g_sa.shown_prompt = prompt;
		}
		return false;
	}
	case SA_LEAVE: {
		uint32_t gone = sa_ease_out(elapsed, SA_LEAVE_US);

		sa_hello_frame(&layout, layout.band, LOGIN_HELLO_LENGTH, 255U - gone, 255U, true);
		return gone == 255U;
	}
	case SA_TO_CARD: {
		uint32_t blur = sa_ease_out(elapsed, SA_TO_CARD_US * 2U / 3U);
		uint64_t rising = elapsed > SA_TO_CARD_US / 4U ? elapsed - SA_TO_CARD_US / 4U : 0U;
		int32_t risen = sa_ease_back(rising, SA_TO_CARD_US * 3U / 4U);

		g_sa.mix = blur;
		g_sa.hello_alpha = 255U - sa_ease_out(elapsed, SA_TO_CARD_US / 3U);
		g_sa.card_alpha = sa_ease_out(rising, SA_TO_CARD_US / 2U);
		g_sa.card_rise = sa_pt(40) - (int32_t)((int64_t)sa_pt(40) * risen / 65536);
		sa_card_frame(now, true);
		if (elapsed >= SA_TO_CARD_US) {
			g_sa.card_rise = 0;
			g_sa.card_alpha = 255U;
			g_sa.hello_alpha = 0U;
			sa_enter(SA_CARD, now);
			*dirty = true;
		}
		return false;
	}
	case SA_CARD: {
		/*
		 * The whole card for a change of page or anything typed or clicked;
		 * otherwise only the parts that move: a switch sliding, the Dock
		 * preview resizing, the caret blinking, the clock ticking. A part
		 * whose place is not known yet repaints the card.
		 */
		bool whole = *dirty || (g_sa.page != g_sa.from_page && now - g_sa.page_at < SA_PAGE_US + SA_FRAME_US);
		bool caret = (g_sa.page == SA_PAGE_ACCOUNT || g_sa.page == SA_PAGE_DEVICE);
		uint64_t blink = (now - g_sa.caret_at) / SA_CARET_US;
		static uint64_t shown_blink = ~0ULL;
		sa_rect_t part = {0, 0, 0, 0};

		for (uint32_t index = 0U; index < 6U; index++) {
			if (g_sa.toggle_at[index] != 0U && now - g_sa.toggle_at[index] < SA_CONTROL_US + SA_FRAME_US) {
				if (sa_empty(g_sa.switch_rects[index])) whole = true;
				part = sa_union(part, g_sa.switch_rects[index]);
			}
		}
		if (g_sa.segment_at != 0U && now - g_sa.segment_at < SA_DOCK_US + SA_FRAME_US) {
			if (sa_empty(g_sa.dock_rect)) whole = true;
			part = sa_union(part, g_sa.dock_rect);
		}
		if (caret && blink != shown_blink) {
			if (sa_empty(g_sa.blink_rect)) whole = true;
			part = sa_union(part, g_sa.blink_rect);
		}
		if (g_sa.page == SA_PAGE_MENUBAR && rtc_unix_time() != g_sa.clock_second) {
			g_sa.clock_second = rtc_unix_time();
			if (sa_empty(g_sa.clock_rect)) whole = true;
			part = sa_union(part, g_sa.clock_rect);
		}

		if (whole || g_sa.repaint_all) {
			sa_card_frame(now, g_sa.repaint_all);
			g_sa.repaint_all = false;
			shown_blink = blink;
			*dirty = false;
		} else if (!sa_empty(part)) {
			sa_card_paint(now, false, part);
			shown_blink = blink;
		}
		/* The change of page is over once its last frame is on screen (it is: moving held until then). */
		if (g_sa.page != g_sa.from_page && now - g_sa.page_at >= SA_PAGE_US + SA_FRAME_US) g_sa.from_page = g_sa.page;

		if (g_sa.page == SA_PAGE_APPLY && g_sa.from_page == SA_PAGE_APPLY) {
			/* "Setting Up" is on screen: do it (tepOS's Argon2id takes a moment). */
			sa_card_frame(now, false);
			if (sa_apply()) {
				sa_enter(SA_TO_DONE, timer_get_microseconds());
			} else {
				g_sa.from_page = SA_PAGE_ACCOUNT;
				g_sa.page = SA_PAGE_ACCOUNT;
				g_sa.focus = SA_FIELD_PASSWORD;
				*dirty = true;
			}
		}
		return false;
	}
	case SA_TO_DONE: {
		uint32_t gone = sa_ease_out(elapsed, SA_TO_DONE_US);

		g_sa.card_alpha = 255U - gone;
		g_sa.card_rise = -(int32_t)((int64_t)sa_pt(24) * gone / 255U);
		g_sa.mix = 255U - gone;
		sa_card_frame(now, true);
		if (gone == 255U) {
			g_sa.card_alpha = 0U;
			g_sa.card_rise = 0;
			g_sa.mix = 0U;
			sa_enter(SA_DONE, now);
			*dirty = true;
		}
		return false;
	}
	}
	return false;
}

static void
sa_reset(display_device_t *display, uint32_t *wallpaper, uint32_t scale_permille)
{
	uint32_t *backdrop = g_sa.backdrop;

	memset(&g_sa, 0, sizeof(g_sa));
	g_sa.backdrop = backdrop;
	g_sa.display = display;
	g_sa.wallpaper = wallpaper;
	g_sa.permille = scale_permille < 500U ? 500U : scale_permille > 4000U ? 4000U : scale_permille;
	g_sa.font_scale = (g_sa.permille + 500U) / 1000U;
	if (g_sa.font_scale < 1U) g_sa.font_scale = 1U;
	g_sa.width = (int32_t)display->width;
	g_sa.height = (int32_t)display->height;
	g_sa.picture = -1;
	g_sa.dock_size = g_sa.dock_size_from = 1U;
	g_sa.toggles[0] = true;	 /* recent apps */
	g_sa.toggles[1] = true;	 /* the Trash */
	g_sa.toggles[2] = true;	 /* the weekday */
	g_sa.toggles[3] = false; /* the date */
	g_sa.toggles[4] = false; /* 24-hour */
	g_sa.toggles[5] = false; /* seconds */
	g_sa.fields[SA_FIELD_USERNAME].capacity = 31U;
	g_sa.fields[SA_FIELD_PASSWORD].capacity = 64U;
	g_sa.fields[SA_FIELD_PASSWORD].secure = true;
	g_sa.fields[SA_FIELD_VERIFY].capacity = 64U;
	g_sa.fields[SA_FIELD_VERIFY].secure = true;
	g_sa.fields[SA_FIELD_DEVICE].capacity = 40U;
}

bool
setup_assistant_run(display_device_t *display, uint32_t *wallpaper, uint32_t scale_permille)
{
	if (display == 0 || display->framebuffer == 0) return false;

	sa_reset(display, wallpaper, scale_permille);
	kprintf("first boot: the setup assistant\n");
	sa_load_clock_offset();
	sa_build_backdrop();

	uint64_t now = timer_get_microseconds();
	bool dirty = true;

	sa_enter(SA_HELLO, now);
	g_sa.page = g_sa.from_page = SA_PAGE_ACCOUNT;

	for (;;) {
		now = timer_get_microseconds();
		if (sa_frame(now, &dirty)) break;

		/* Input until the next frame. */
		uint64_t next = now + SA_FRAME_US;
		do {
			int32_t x, y;
			uint32_t pressed = 0U;
			keyboard_key_t key;
			uint64_t at = timer_get_microseconds();

			virtio_input_service();
			if (login_pointer_poll(display, &x, &y, &pressed)) {
				if ((pressed & 1U) && !(g_sa.pressed & 1U)) {
					int32_t id = sa_hit_at(x, y);

					if (g_sa.phase == SA_HELLO && id == SA_HIT_GET_STARTED) {
						sa_enter(SA_TO_CARD, at);
					} else if (g_sa.phase == SA_DONE && id == SA_HIT_FINISH) {
						sa_enter(SA_LEAVE, at);
					} else if (g_sa.phase == SA_CARD && id != 0) {
						sa_card_click(id, at);
						dirty = true;
					}
				}
				g_sa.pressed = pressed;
			}

			if (keyboard_take_key(&key)) {
				bool go = key.code == 28 || key.character == ' ';

				if (g_sa.phase == SA_HELLO && go && at - g_sa.phase_at >= SA_PROMPT_AT_US) {
					sa_enter(SA_TO_CARD, at);
				} else if (g_sa.phase == SA_DONE && go && at - g_sa.phase_at >= SA_PROMPT_AT_US) {
					sa_enter(SA_LEAVE, at);
				} else if (g_sa.phase == SA_CARD && g_sa.page < SA_PAGES) {
					sa_card_key(&key, at);
					dirty = true;
				}
				sa_wipe(&key, sizeof(key));
			}

			bool still = (g_sa.phase == SA_HELLO || g_sa.phase == SA_DONE) && g_sa.shown_prompt == 255U;
			(void)sched_sleep_us(still ? 8000U : 3000U);
		} while (timer_get_microseconds() < next && !dirty);
	}

	/* The password is in tepOS and the files: nothing of it stays here. */
	sa_wipe(g_sa.fields, sizeof(g_sa.fields));
	for (uint32_t index = 0U; index < g_sa.picture_count; index++)
		if (g_sa.pictures[index].thumbnail != 0) (void)kfree(g_sa.pictures[index].thumbnail);
	if (g_sa.backdrop != 0) (void)kfree(g_sa.backdrop);
	g_sa.backdrop = 0;
	if (g_sa.card_base != 0) (void)kfree(g_sa.card_base);
	g_sa.card_base = 0;
	kprintf("setup finished: on to the desktop\n");
	return true;
}

void
setup_assistant_draw_island(display_device_t *display, uint32_t scale_permille)
{
	if (display == 0 || display->framebuffer == 0) return;
	g_sa.display = display;
	g_sa.width = (int32_t)display->width;
	g_sa.height = (int32_t)display->height;
	g_sa.permille = scale_permille < 500U ? 500U : scale_permille > 4000U ? 4000U : scale_permille;
	g_sa.clip = sa_screen();
	sa_island();
}

bool
setup_assistant_forget_welcome(void)
{
	vnode_t vnode;

	if (vfs_lookup(SA_VOLUME "/Library", &vnode) != VFS_STATUS_OK) return false;
	vnode_rele(vnode);
	if (vfs_lookup(SA_WELCOME_PLIST, &vnode) != VFS_STATUS_OK) return true;
	vnode_rele(vnode);
	if (vfs_unlink(SA_WELCOME_PLIST) == VFS_STATUS_OK) kprintf("the welcome has been seen\n");
	return true;
}
