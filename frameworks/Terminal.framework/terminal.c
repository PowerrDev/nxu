/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 *
 * A desktop terminal for nzh with macOS-style tabs. Each tab owns an nzh shell
 * over a local stream socket (canonical input, redirected child output). The GUI
 * reads every tab's shell each loop so background tabs keep running; only the
 * active tab's screen is drawn. A tab bar (active pill, flat tabs with dividers,
 * hover-reveal close, a busy spinner and a "+") sits below the titlebar.
 */
#include "shell.h"

#if defined(__i386__)
#include <nxu/spawn_io.h>
#endif

#include "terminal_font.h"
#include <nxu/string.h>
#include <nxu/syscall.h>

#define TERMINAL_COLUMNS 80U
#define TERMINAL_ROWS 24U
#define TERMINAL_HISTORY_ROWS 256U
#define TERMINAL_WIDTH 672U
#define TERMINAL_HEIGHT 416U
#define TERMINAL_COMMAND_HISTORY 32U

/* The tab bar occupies the top of the content area; the grid sits below it. */
#define TAB_BAR_HEIGHT 37U
#define CONTENT_TOP TAB_BAR_HEIGHT
#define FRAME_HEIGHT (TAB_BAR_HEIGHT + TERMINAL_HEIGHT)
#define TITLEBAR_HEIGHT 24U

#define TABS_MAX 8U
#define CELL_W 8U
#define CELL_H 16U

/* Colours (dark, matching macOS Terminal's "Pro"-ish chrome). */
#define COLOR_TERM_BG 0xff151a22U
#define COLOR_TEXT 0xffdce4eeU
#define COLOR_INPUT 0xffa7e3b0U
#define COLOR_BAR_BG 0xff2a2a2dU
#define COLOR_PILL 0xff4a4a4eU
#define COLOR_DIVIDER 0xff3c3c40U
#define COLOR_TITLE_ON 0xffe6e6e8U
#define COLOR_TITLE_OFF 0xff8c8c90U
#define COLOR_GLYPH_CHROME 0xffc8c8ccU

static uint32_t g_pixels[TERMINAL_WIDTH * FRAME_HEIGHT];

typedef struct {
	bool used;
	int64_t shell_pid;
	int64_t stream;

	char rows[TERMINAL_HISTORY_ROWS][TERMINAL_COLUMNS];
	uint64_t row;
	uint32_t column;
	uint32_t scroll;

	char line[NZH_LINE_MAX];
	uint32_t length;
	uint32_t cursor;

	char history[TERMINAL_COMMAND_HISTORY][NZH_LINE_MAX];
	uint32_t history_count;
	uint32_t history_position;
	char saved_line[NZH_LINE_MAX];

	char escape[8];
	uint32_t escape_length;
	bool escaping;

	/* Spinner: loop-frames since the shell last produced output. */
	uint64_t last_output_frame;
} terminal_tab_t;

static terminal_tab_t g_tabs[TABS_MAX];
static uint32_t g_tab_count;
static uint32_t g_active;

static bool g_dirty = true;
static uint64_t g_frame; /* advances once per event-loop iteration */

/* Hover state for the tab bar, refreshed on every pointer move. */
static int32_t g_hover_tab = -1;  /* tab under the pointer, or -1 */
static bool g_hover_close = false; /* pointer is over that tab's close affordance */
static bool g_hover_plus = false;

static char g_socket_name[64];

static terminal_tab_t *
active_tab(void)
{
	return &g_tabs[g_active];
}

/* ------------------------------------------------------------------------- */
/* Terminal emulation (per tab).                                             */
/* ------------------------------------------------------------------------- */

static void
terminal_append(terminal_tab_t *tab, char character)
{
	if (tab->escaping) {
		if (tab->escape_length + 1 < sizeof(tab->escape)) {
			tab->escape[tab->escape_length++] = character;
			tab->escape[tab->escape_length] = 0;
		}

		if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
			tab->escape_length + 1 == sizeof(tab->escape)) {
			if (nxu_streq(tab->escape, "[2J")) {
				memset(tab->rows, ' ', sizeof(tab->rows));
				tab->row = 0;
				tab->column = 0;
				tab->scroll = 0;
			}

			tab->escaping = false;
			tab->escape_length = 0;
		}

		return;
	}

	if (character == '\033') {
		tab->escaping = true;
		tab->escape_length = 0;
		return;
	}

	if (character == '\r') {
		tab->column = 0;
		return;
	}

	if (character == '\n' || tab->column == TERMINAL_COLUMNS) {
		tab->row++;
		tab->column = 0;
		memset(tab->rows[tab->row % TERMINAL_HISTORY_ROWS], ' ', TERMINAL_COLUMNS);
	}

	if (character != '\n' && (uint8_t)character >= 32) {
		tab->rows[tab->row % TERMINAL_HISTORY_ROWS][tab->column++] = character;
	}

	g_dirty = true;
}

static void
terminal_text(terminal_tab_t *tab, const char *text)
{
	while (*text != 0) {
		terminal_append(tab, *text++);
	}
}

/* ------------------------------------------------------------------------- */
/* Pixel helpers.                                                            */
/* ------------------------------------------------------------------------- */

/* `foreground` over `background` by an 8-bit coverage, straight alpha. */
static uint32_t
terminal_blend(uint32_t background, uint32_t foreground, uint8_t coverage)
{
	uint32_t inverse = 255U - coverage;
	uint32_t red = (((foreground >> 16) & 0xFFU) * coverage + ((background >> 16) & 0xFFU) * inverse) / 255U;
	uint32_t green = (((foreground >> 8) & 0xFFU) * coverage + ((background >> 8) & 0xFFU) * inverse) / 255U;
	uint32_t blue = ((foreground & 0xFFU) * coverage + (background & 0xFFU) * inverse) / 255U;
	return 0xFF000000U | (red << 16) | (green << 8) | blue;
}

static void
blend_at(int32_t x, int32_t y, uint32_t color, uint8_t coverage)
{
	if (x < 0 || x >= (int32_t)TERMINAL_WIDTH || y < 0 || y >= (int32_t)FRAME_HEIGHT || coverage == 0U) {
		return;
	}

	uint32_t *pixel = &g_pixels[(uint32_t)y * TERMINAL_WIDTH + (uint32_t)x];
	*pixel = coverage == 255U ? color : terminal_blend(*pixel, color, coverage);
}

static void
fill_rect(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color)
{
	for (int32_t row = 0; row < height; row++) {
		for (int32_t column = 0; column < width; column++) {
			blend_at(x + column, y + row, color, 255U);
		}
	}
}

/* A filled rounded rectangle: the four corners are clipped to a circle. */
static void
fill_rounded(int32_t x, int32_t y, int32_t width, int32_t height, int32_t radius, uint32_t color)
{
	for (int32_t row = 0; row < height; row++) {
		for (int32_t column = 0; column < width; column++) {
			int32_t dx = 0;
			int32_t dy = 0;

			if (column < radius) {
				dx = radius - column;
			} else if (column >= width - radius) {
				dx = column - (width - radius - 1);
			}
			if (row < radius) {
				dy = radius - row;
			} else if (row >= height - radius) {
				dy = row - (height - radius - 1);
			}

			if (dx != 0 && dy != 0 && dx * dx + dy * dy > radius * radius) {
				continue;
			}

			blend_at(x + column, y + row, color, 255U);
		}
	}
}

/* Draw one SF Mono glyph with `x`,`y` the cell's top-left (antialiased). */
static void
terminal_glyph(int32_t x, int32_t y, uint8_t character, uint32_t color)
{
	if (character < TERMINAL_FONT_FIRST || character > TERMINAL_FONT_LAST) {
		return;
	}

	const terminal_font_glyph_t *glyph = &g_terminal_font_glyphs[character - TERMINAL_FONT_FIRST];
	const uint8_t *coverage = &g_terminal_font_pixels[glyph->offset];

	for (uint32_t row = 0; row < glyph->height; row++) {
		for (uint32_t column = 0; column < glyph->width; column++) {
			uint8_t alpha = coverage[row * glyph->width + column];
			blend_at(x + glyph->x_offset + (int32_t)column, y + glyph->y_offset + (int32_t)row, color, alpha);
		}
	}
}

static void
terminal_string(int32_t x, int32_t y, const char *text, uint32_t color)
{
	while (*text != 0) {
		terminal_glyph(x, y, (uint8_t)*text, color);
		x += TERMINAL_FONT_ADVANCE;
		text++;
	}
}

static uint32_t
string_width(const char *text)
{
	return (uint32_t)nxu_strlen(text) * TERMINAL_FONT_ADVANCE;
}

/* ------------------------------------------------------------------------- */
/* Tab bar chrome.                                                           */
/* ------------------------------------------------------------------------- */

/* A small "x" close glyph centred at (cx, cy). */
static void
draw_close(int32_t cx, int32_t cy, uint32_t color)
{
	for (int32_t d = -3; d <= 3; d++) {
		blend_at(cx + d, cy + d, color, 230U);
		blend_at(cx + d, cy - d, color, 230U);
		blend_at(cx + d + 1, cy + d, color, 120U);
		blend_at(cx + d - 1, cy + d, color, 120U);
	}
}

/* A "+" centred at (cx, cy). */
static void
draw_plus(int32_t cx, int32_t cy, uint32_t color)
{
	for (int32_t d = -5; d <= 5; d++) {
		blend_at(cx + d, cy, color, 235U);
		blend_at(cx, cy + d, color, 235U);
		blend_at(cx + d, cy + 1, color, 90U);
		blend_at(cx + 1, cy + d, color, 90U);
	}
}

/* An eight-spoke spinner centred at (cx, cy); the bright spoke rotates. */
static void
draw_spinner(int32_t cx, int32_t cy, uint32_t color)
{
	/* Spokes at the eight principal directions, small integer offsets. */
	static const int8_t sx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
	static const int8_t sy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
	uint32_t head = (uint32_t)((g_frame / 3U) % 8U);

	for (uint32_t spoke = 0; spoke < 8U; spoke++) {
		/* Trailing fade behind the head. */
		uint32_t age = (head + 8U - spoke) % 8U;
		uint8_t coverage = (uint8_t)(230U - age * 26U);

		for (int32_t step = 2; step <= 4; step++) {
			int32_t px = cx + sx[spoke] * step / 4;
			int32_t py = cy + sy[spoke] * step / 4;
			blend_at(px, py, color, coverage);
		}
	}
}

/* Pixel geometry of the tab bar, derived from the current tab count. */
static int32_t
plus_center_x(void)
{
	return (int32_t)TERMINAL_WIDTH - 22;
}

static int32_t
tabs_right_edge(void)
{
	return plus_center_x() - 20;
}

static int32_t
tab_width(void)
{
	int32_t span = tabs_right_edge() - 8;
	uint32_t count = g_tab_count == 0 ? 1U : g_tab_count;
	return span / (int32_t)count;
}

static int32_t
tab_x(uint32_t index)
{
	return 8 + (int32_t)index * tab_width();
}

static void
draw_tab_bar(void)
{
	fill_rect(0, 0, (int32_t)TERMINAL_WIDTH, (int32_t)TAB_BAR_HEIGHT, COLOR_BAR_BG);
	/* A hairline under the bar. */
	fill_rect(0, (int32_t)TAB_BAR_HEIGHT - 1, (int32_t)TERMINAL_WIDTH, 1, COLOR_DIVIDER);

	int32_t width = tab_width();
	int32_t top = 4;
	int32_t height = (int32_t)TAB_BAR_HEIGHT - 9;
	int32_t mid = (int32_t)TAB_BAR_HEIGHT / 2;

	for (uint32_t index = 0; index < g_tab_count; index++) {
		int32_t x = tab_x(index);
		bool is_active = index == g_active;
		bool is_hovered = g_hover_tab == (int32_t)index;

		if (is_active) {
			/* The active tab is a raised, rounded pill. */
			fill_rounded(x + 2, top, width - 4, height, 7, COLOR_PILL);
		} else if (index + 1 < g_tab_count && index + 1 != g_active) {
			/* A divider between two inactive tabs. */
			fill_rect(x + width - 1, top + 3, 1, height - 6, COLOR_DIVIDER);
		}

		/* Title, centred: "~ - nzh" (ASCII em-dash substitute). */
		const char *title = "~ - nzh";
		int32_t title_w = (int32_t)string_width(title);
		int32_t title_x = x + (width - title_w) / 2;
		terminal_string(title_x, mid - (int32_t)CELL_H / 2, title, is_active ? COLOR_TITLE_ON : COLOR_TITLE_OFF);

		/* Close affordance on hover (left side, macOS-style). */
		if (is_hovered || is_active) {
			uint32_t close_color = (is_hovered && g_hover_close) ? COLOR_TITLE_ON : COLOR_TITLE_OFF;
			if (is_hovered) {
				draw_close(x + 14, mid, close_color);
			}
		}

		/* Busy spinner on the right while the shell produces output. */
		if (g_frame - g_tabs[index].last_output_frame < 25U) {
			draw_spinner(x + width - 16, mid, is_active ? COLOR_TITLE_ON : COLOR_TITLE_OFF);
		}
	}

	/* The "+" button in its own circle at the far right. */
	int32_t px = plus_center_x();
	fill_rounded(px - 13, mid - 13, 26, 26, 13, g_hover_plus ? COLOR_PILL : COLOR_BAR_BG);
	draw_plus(px, mid, COLOR_GLYPH_CHROME);
}

/* ------------------------------------------------------------------------- */
/* Terminal content (active tab).                                            */
/* ------------------------------------------------------------------------- */

static void
draw_content(terminal_tab_t *tab)
{
	fill_rect(0, (int32_t)CONTENT_TOP, (int32_t)TERMINAL_WIDTH, (int32_t)TERMINAL_HEIGHT, COLOR_TERM_BG);

	uint64_t last = tab->row >= tab->scroll ? tab->row - tab->scroll : 0;
	if (tab->scroll == 0) {
		last += (tab->column + tab->length) / TERMINAL_COLUMNS;
	}

	uint64_t first = last >= TERMINAL_ROWS - 1 ? last - TERMINAL_ROWS + 1 : 0;

	for (uint32_t row = 0; row < TERMINAL_ROWS && first + row <= last && first + row <= tab->row; row++) {
		const char *text = tab->rows[(first + row) % TERMINAL_HISTORY_ROWS];

		for (uint32_t column = 0; column < TERMINAL_COLUMNS; column++) {
			terminal_glyph(16 + (int32_t)(column * CELL_W), (int32_t)CONTENT_TOP + 16 + (int32_t)(row * CELL_H),
						   (uint8_t)text[column], COLOR_TEXT);
		}
	}

	if (tab->scroll == 0) {
		for (uint32_t index = 0; index <= tab->length; index++) {
			uint32_t position = tab->column + index;
			uint64_t row = tab->row - first + position / TERMINAL_COLUMNS;
			uint32_t column = position % TERMINAL_COLUMNS;

			if (row >= TERMINAL_ROWS) {
				break;
			}

			int32_t cx = 16 + (int32_t)(column * CELL_W);
			int32_t cy = (int32_t)CONTENT_TOP + 16 + (int32_t)(row * CELL_H);

			if (index < tab->length) {
				terminal_glyph(cx, cy, (uint8_t)tab->line[index], COLOR_INPUT);
			}

			if (index == tab->cursor) {
				fill_rect(cx, cy, (int32_t)CELL_W, (int32_t)CELL_H, COLOR_INPUT);
				if (index < tab->length) {
					terminal_glyph(cx, cy, (uint8_t)tab->line[index], COLOR_TERM_BG);
				}
			}
		}
	}
}

static void
terminal_draw(uint32_t connection)
{
	draw_content(active_tab());
	draw_tab_bar();

	nxu_ui_submit_t submit = {.pixels = (uint64_t)(uintptr_t)g_pixels,
							  .width = TERMINAL_WIDTH,
							  .height = FRAME_HEIGHT,
							  .stride = TERMINAL_WIDTH};
	(void)nxu_ui_submit(connection, &submit);
	g_dirty = false;
}

/* ------------------------------------------------------------------------- */
/* Input editing (active tab).                                               */
/* ------------------------------------------------------------------------- */

static void
terminal_recall(terminal_tab_t *tab, bool previous)
{
	if (tab->history_count == 0) {
		return;
	}

	if (tab->history_position == tab->history_count) {
		memcpy(tab->saved_line, tab->line, sizeof(tab->line));
	}

	if (previous && tab->history_position > 0) {
		tab->history_position--;
	} else if (!previous && tab->history_position < tab->history_count) {
		tab->history_position++;
	}

	memcpy(tab->line,
		   tab->history_position == tab->history_count ? tab->saved_line
													   : tab->history[tab->history_position % TERMINAL_COMMAND_HISTORY],
		   sizeof(tab->line));
	tab->length = nxu_strlen(tab->line);
	tab->cursor = tab->length;
}

static void
terminal_complete(terminal_tab_t *tab)
{
	for (uint32_t index = 0; index < tab->length; index++) {
		if (tab->line[index] == ' ') {
			return;
		}
	}

	int64_t descriptor = nxu_open("/Volumes/mainOS/System/Library/CoreServices", NXU_O_READ);

	if (descriptor < 0) {
		return;
	}

	nxu_dirent_t entry;
	char match[NZH_LINE_MAX] = {0};
	uint32_t matches = 0;

	while (nxu_readdir(descriptor, &entry) > 0) {
		uint32_t index = 0;

		while (index < tab->length && entry.name[index] == tab->line[index]) {
			index++;
		}

		if (index == tab->length && entry.name[0] != '.' && nxu_strlen(entry.name) + 2 < sizeof(match)) {
			memcpy(match, entry.name, nxu_strlen(entry.name) + 1);
			matches++;
		}
	}

	(void)nxu_close(descriptor);

	if (matches == 1) {
		tab->length = nxu_strlen(match);
		memcpy(tab->line, match, tab->length);
		tab->line[tab->length++] = ' ';
		tab->line[tab->length] = 0;
		tab->cursor = tab->length;
	}
}

/* Returns false if the tab's shell connection is gone (the tab should close). */
static bool
terminal_key(terminal_tab_t *tab, const nxu_ui_message_t *message)
{
	uint32_t key = message->button;
	uint32_t character = message->character;

	if (character == 3) {
#if defined(__i386__)
		char interrupt = 3;
		(void)nxu_write(tab->stream, &interrupt, 1);
#else
		(void)nxu_kill(tab->shell_pid, NXU_SIGINT);
#endif
		tab->length = 0;
		tab->cursor = 0;
		tab->line[0] = 0;
		terminal_text(tab, "^C\n");
	} else if (key == 103 || key == 108) {
		terminal_recall(tab, key == 103);
	} else if (key == 105 && tab->cursor > 0) {
		tab->cursor--;
	} else if (key == 106 && tab->cursor < tab->length) {
		tab->cursor++;
	} else if (key == 102) {
		tab->cursor = 0;
	} else if (key == 107) {
		tab->cursor = tab->length;
	} else if ((key == 14 || character == 8 || character == 127) && tab->cursor > 0) {
		memmove(tab->line + tab->cursor - 1, tab->line + tab->cursor, tab->length - tab->cursor + 1);
		tab->cursor--;
		tab->length--;
	} else if (key == 111 && tab->cursor < tab->length) {
		memmove(tab->line + tab->cursor, tab->line + tab->cursor + 1, tab->length - tab->cursor);
		tab->length--;
	} else if (key == 15 || character == '\t') {
		terminal_complete(tab);
	} else if (key == 28 || character == '\n' || character == '\r') {
		tab->line[tab->length] = 0;
		terminal_text(tab, tab->line);
		terminal_append(tab, '\n');

		if (tab->length != 0) {
			if (tab->history_count == TERMINAL_COMMAND_HISTORY) {
				memmove(tab->history[0], tab->history[1], sizeof(tab->history) - sizeof(tab->history[0]));
				tab->history_count--;
			}
			memcpy(tab->history[tab->history_count++], tab->line, tab->length + 1);
		}

		tab->history_position = tab->history_count;
		tab->line[tab->length++] = '\n';
		if (nxu_write(tab->stream, tab->line, tab->length) != (int64_t)tab->length) {
			return false;
		}
		tab->last_output_frame = g_frame; /* a command was launched */

		tab->length = 0;
		tab->cursor = 0;
		tab->line[0] = 0;
	} else if (character >= 32 && character < 127 && tab->length + 2 < sizeof(tab->line)) {
		memmove(tab->line + tab->cursor + 1, tab->line + tab->cursor, tab->length - tab->cursor + 1);
		tab->line[tab->cursor++] = character;
		tab->length++;
	}

	tab->scroll = 0;
	g_dirty = true;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Tab lifecycle.                                                            */
/* ------------------------------------------------------------------------- */

/* Spawn an nzh shell for `tab`; `index` disambiguates its socket. */
static bool
spawn_shell(terminal_tab_t *tab, uint32_t index)
{
	memset(tab, 0, sizeof(*tab));
	memset(tab->rows, ' ', sizeof(tab->rows));
	tab->stream = -1;

#if defined(__i386__)
	(void)index;
	const char *arguments[] = {"nzh", 0};
	int64_t stream = -1;
	int64_t shell_pid = nxu_spawn_io("/Volumes/mainOS/System/Library/CoreServices/nzh", arguments, &stream);

	if (shell_pid <= 0 || stream < 0) {
		return false;
	}
#else
	char name[64];
	uint32_t length = 0;
	for (const char *source = g_socket_name; *source != 0; source++) {
		name[length++] = *source;
	}
	name[length++] = '.';
	name[length++] = "0123456789abcdef"[index & 15U];
	name[length] = 0;

	int64_t listener = nxu_socket_listen(name, 1);
	if (listener < 0) {
		return false;
	}

	int64_t shell_pid = nxu_fork();
	if (shell_pid == 0) {
		(void)nxu_close(listener);
		int64_t stream = nxu_socket_connect(name);

		if (stream < 0 || nxu_dup2(stream, 0) < 0 || nxu_dup2(stream, 1) < 0 || nxu_dup2(stream, 2) < 0) {
			nxu_exit(2);
		}
		(void)nxu_close(stream);
		(void)nxu_exec("/Volumes/mainOS/System/Library/CoreServices/nzh", 0);
		nxu_exit(3);
	}

	if (shell_pid < 0) {
		(void)nxu_close(listener);
		return false;
	}

	int64_t stream = nxu_socket_accept(listener);
	(void)nxu_close(listener);
	if (stream < 0) {
		(void)nxu_kill(shell_pid, NXU_SIGTERM);
		return false;
	}
#endif

	tab->used = true;
	tab->shell_pid = shell_pid;
	tab->stream = stream;
	return true;
}

static void
close_tab(uint32_t index)
{
	if (index >= g_tab_count) {
		return;
	}

	terminal_tab_t *tab = &g_tabs[index];
	(void)nxu_close(tab->stream);
	(void)nxu_kill(tab->shell_pid, NXU_SIGTERM);
	uint64_t status;
	(void)nxu_wait(tab->shell_pid, &status);

	for (uint32_t move = index; move + 1 < g_tab_count; move++) {
		g_tabs[move] = g_tabs[move + 1];
	}
	g_tab_count--;
	memset(&g_tabs[g_tab_count], 0, sizeof(g_tabs[g_tab_count]));

	if (g_active >= g_tab_count && g_tab_count != 0) {
		g_active = g_tab_count - 1;
	} else if (g_active > index) {
		g_active--;
	}
	g_dirty = true;
}

static void
new_tab(void)
{
	if (g_tab_count >= TABS_MAX) {
		return;
	}

	if (!spawn_shell(&g_tabs[g_tab_count], g_tab_count)) {
		return;
	}

	g_active = g_tab_count;
	g_tab_count++;
	g_dirty = true;
}

/* ------------------------------------------------------------------------- */
/* Pointer handling for the tab bar.                                         */
/* ------------------------------------------------------------------------- */

/* Which tab (if any) a point falls in; -1 otherwise. */
static int32_t
tab_at(int32_t x, int32_t y)
{
	if (y < 0 || y >= (int32_t)TAB_BAR_HEIGHT) {
		return -1;
	}

	int32_t width = tab_width();
	for (uint32_t index = 0; index < g_tab_count; index++) {
		int32_t left = tab_x(index);
		if (x >= left && x < left + width) {
			return (int32_t)index;
		}
	}
	return -1;
}

static bool
over_plus(int32_t x, int32_t y)
{
	int32_t cx = plus_center_x();
	int32_t cy = (int32_t)TAB_BAR_HEIGHT / 2;
	int32_t dx = x - cx;
	int32_t dy = y - cy;
	return dx * dx + dy * dy <= 15 * 15;
}

/* The close affordance of tab `index` sits on its left. */
static bool
over_close(uint32_t index, int32_t x, int32_t y)
{
	int32_t cx = tab_x(index) + 14;
	int32_t cy = (int32_t)TAB_BAR_HEIGHT / 2;
	int32_t dx = x - cx;
	int32_t dy = y - cy;
	return dx * dx + dy * dy <= 8 * 8;
}

static void
pointer_moved(int32_t x, int32_t y)
{
	int32_t tab = tab_at(x, y);
	bool plus = over_plus(x, y);
	bool close = tab >= 0 && over_close((uint32_t)tab, x, y);

	if (tab != g_hover_tab || plus != g_hover_plus || close != g_hover_close) {
		g_hover_tab = tab;
		g_hover_plus = plus;
		g_hover_close = close;
		g_dirty = true;
	}
}

static void
pointer_down(int32_t x, int32_t y)
{
	if (over_plus(x, y)) {
		new_tab();
		return;
	}

	int32_t tab = tab_at(x, y);
	if (tab < 0) {
		return;
	}

	if (over_close((uint32_t)tab, x, y) && g_tab_count > 1) {
		close_tab((uint32_t)tab);
		return;
	}

	if ((uint32_t)tab != g_active) {
		g_active = (uint32_t)tab;
		g_dirty = true;
	}
}

/* ------------------------------------------------------------------------- */
/* Main.                                                                     */
/* ------------------------------------------------------------------------- */

int
main(void)
{
	/* A bounded, unique local rendezvous base from this process's PID. */
	uint32_t length = 0;
	for (const char *source = "com.nxu.terminal."; *source != 0; source++) {
		g_socket_name[length++] = *source;
	}
	uint32_t pid = nxu_getpid();
	for (int32_t shift = 28; shift >= 0; shift -= 4) {
		g_socket_name[length++] = "0123456789abcdef"[(pid >> shift) & 15U];
	}
	g_socket_name[length] = 0;

	if (!spawn_shell(&g_tabs[0], 0)) {
		return 4;
	}
	g_tab_count = 1;
	g_active = 0;

	nxu_ui_session_t session;
	while (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)(uintptr_t)&session) < 0) {
		(void)nxu_sleep_us(10000);
	}

	nxu_ui_connect_t info = {.struct_size = sizeof(info),
							 .kind = NXU_UI_KIND_APP,
							 .width = TERMINAL_WIDTH,
							 .height = FRAME_HEIGHT + TITLEBAR_HEIGHT,
							 .min_width = TERMINAL_WIDTH,
							 .min_height = FRAME_HEIGHT + TITLEBAR_HEIGHT,
							 .max_width = TERMINAL_WIDTH,
							 .max_height = FRAME_HEIGHT + TITLEBAR_HEIGHT,
							 .titlebar_height = TITLEBAR_HEIGHT,
							 .corner_radius = 24};
	memcpy(info.name, "Terminal", sizeof("Terminal"));
	memcpy(info.bundle_path, "/Applications/Terminal.app", sizeof("/Applications/Terminal.app"));
	int64_t connection = nxu_ui_connect(&info);

	if (connection <= 0) {
		close_tab(0);
		return 6;
	}

	bool running = true;

	while (running && g_tab_count != 0) {
		g_frame++;

		/* Read every tab's shell so background tabs keep running. */
		for (uint32_t index = 0; index < g_tab_count;) {
			terminal_tab_t *tab = &g_tabs[index];
			char output[512];
			bool closed = false;

			for (uint32_t budget = 0; budget < 16; budget++) {
				int64_t count = nxu_read_poll(tab->stream, output, sizeof(output));

				if (count == -NXU_SYS_E_AGAIN) {
					break;
				}
				if (count <= 0) {
					/* The shell exited: drop this tab (quit if it was the last). */
					if (g_tab_count == 1) {
						running = false;
					} else {
						close_tab(index);
					}
					closed = true;
					break;
				}

				for (int64_t at = 0; at < count; at++) {
					terminal_append(tab, output[at]);
				}
				tab->last_output_frame = g_frame;
				if (index == g_active) {
					g_dirty = true;
				}
			}

			if (!closed) {
				index++;
			}
		}

		if (!running) {
			break;
		}

		/* Spinners animate, so redraw while any tab is busy. */
		for (uint32_t index = 0; index < g_tab_count; index++) {
			if (g_frame - g_tabs[index].last_output_frame < 25U) {
				g_dirty = true;
				break;
			}
		}

		if (g_dirty) {
			terminal_draw(connection);
		}

		nxu_ui_message_t message;
		int64_t result = nxu_ui_receive(connection, &message, 16);

		if (result < 0 || (result > 0 && message.type == NXU_UI_MSG_QUIT)) {
			break;
		}
		if (result <= 0) {
			continue;
		}

		if (message.type == NXU_UI_MSG_REDRAW) {
			g_dirty = true;
		} else if (message.type == NXU_UI_MSG_EVENT) {
			switch (message.event) {
			case NXU_UI_EVENT_KEY_DOWN:
				if (!terminal_key(active_tab(), &message)) {
					if (g_tab_count == 1) {
						running = false;
					} else {
						close_tab(g_active);
					}
				}
				break;
			case NXU_UI_EVENT_POINTER_MOVED:
				pointer_moved(message.x, message.y);
				break;
			case NXU_UI_EVENT_POINTER_DOWN:
				pointer_down(message.x, message.y);
				break;
			case NXU_UI_EVENT_POINTER_LEFT:
				if (g_hover_tab != -1 || g_hover_plus || g_hover_close) {
					g_hover_tab = -1;
					g_hover_plus = false;
					g_hover_close = false;
					g_dirty = true;
				}
				break;
			case NXU_UI_EVENT_SCROLL: {
				terminal_tab_t *tab = active_tab();
				int32_t scroll = (int32_t)tab->scroll + (message.delta > 0 ? 3 : -3);
				uint32_t limit =
					tab->row < TERMINAL_HISTORY_ROWS - TERMINAL_ROWS ? (uint32_t)tab->row : TERMINAL_HISTORY_ROWS - TERMINAL_ROWS;
				tab->scroll = scroll < 0 ? 0 : (uint32_t)scroll > limit ? limit : (uint32_t)scroll;
				g_dirty = true;
				break;
			}
			default:
				break;
			}
		}
	}

	while (g_tab_count != 0) {
		close_tab(g_tab_count - 1);
	}
	return 0;
}
