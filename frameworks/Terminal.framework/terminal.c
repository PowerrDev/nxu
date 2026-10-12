/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 *
 * A desktop terminal for nzh. Local stream sockets carry canonical input and
 * redirected child output; the GUI remains responsive while commands run.
 */
#include "shell.h"

#if defined(__i386__)
#include <nxu/spawn_io.h>
#endif

#include <kern/console/font8x16.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#define TERMINAL_COLUMNS 80U
#define TERMINAL_ROWS 24U
#define TERMINAL_HISTORY_ROWS 256U
#define TERMINAL_WIDTH 672U
#define TERMINAL_HEIGHT 416U
#define TERMINAL_COMMAND_HISTORY 32U

static uint32_t g_pixels[TERMINAL_WIDTH * TERMINAL_HEIGHT];
static char g_rows[TERMINAL_HISTORY_ROWS][TERMINAL_COLUMNS];
static uint64_t g_row;
static uint32_t g_column;
static uint32_t g_scroll;
static char g_line[NZH_LINE_MAX];
static uint32_t g_length;
static uint32_t g_cursor;
static char g_history[TERMINAL_COMMAND_HISTORY][NZH_LINE_MAX];
static uint32_t g_history_count;
static uint32_t g_history_position;
static char g_saved_line[NZH_LINE_MAX];
static char g_escape[8];
static uint32_t g_escape_length;
static bool g_escaping;
static bool g_dirty = true;

static void
terminal_append(char character)
{
	if (g_escaping) {
		if (g_escape_length + 1 < sizeof(g_escape)) {
			g_escape[g_escape_length++] = character;
			g_escape[g_escape_length] = 0;
		}

		if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
			g_escape_length + 1 == sizeof(g_escape)) {
			if (nxu_streq(g_escape, "[2J")) {
				memset(g_rows, ' ', sizeof(g_rows));
				g_row = 0;
				g_column = 0;
				g_scroll = 0;
			}

			g_escaping = false;
			g_escape_length = 0;
		}

		return;
	}

	if (character == '\033') {
		g_escaping = true;
		g_escape_length = 0;
		return;
	}

	if (character == '\r') {
		g_column = 0;
		return;
	}

	if (character == '\n' || g_column == TERMINAL_COLUMNS) {
		g_row++;
		g_column = 0;
		memset(g_rows[g_row % TERMINAL_HISTORY_ROWS], ' ', TERMINAL_COLUMNS);
	}

	if (character != '\n' && (uint8_t)character >= 32) {
		g_rows[g_row % TERMINAL_HISTORY_ROWS][g_column++] = character;
	}

	g_dirty = true;
}

static void
terminal_text(const char *text)
{
	while (*text != 0) {
		terminal_append(*text++);
	}
}

static void
terminal_glyph(uint32_t x, uint32_t y, uint8_t character, uint32_t color)
{
	const uint8_t *glyph = &g_font8x16[(uint32_t)character * FONT8X16_HEIGHT];

	for (uint32_t row = 0; row < FONT8X16_HEIGHT && y + row < TERMINAL_HEIGHT; row++) {
		for (uint32_t column = 0; column < FONT8X16_WIDTH && x + column < TERMINAL_WIDTH; column++) {
			if (glyph[row] & (1U << column)) {
				g_pixels[(y + row) * TERMINAL_WIDTH + x + column] = color;
			}
		}
	}
}

static void
terminal_draw(uint32_t connection)
{
	for (uint32_t pixel = 0; pixel < TERMINAL_WIDTH * TERMINAL_HEIGHT; pixel++) {
		g_pixels[pixel] = 0xff151a22;
	}

	uint64_t last = g_row >= g_scroll ? g_row - g_scroll : 0;
	if (g_scroll == 0) {
		last += (g_column + g_length) / TERMINAL_COLUMNS;
	}

	uint64_t first = last >= TERMINAL_ROWS - 1 ? last - TERMINAL_ROWS + 1 : 0;

	for (uint32_t row = 0; row < TERMINAL_ROWS && first + row <= last && first + row <= g_row; row++) {
		const char *text = g_rows[(first + row) % TERMINAL_HISTORY_ROWS];

		for (uint32_t column = 0; column < TERMINAL_COLUMNS; column++) {
			terminal_glyph(16 + column * 8, 16 + row * 16, text[column], 0xffdce4ee);
		}
	}

	if (g_scroll == 0) {
		for (uint32_t index = 0; index <= g_length; index++) {
			uint32_t position = g_column + index;
			uint64_t row = g_row - first + position / TERMINAL_COLUMNS;
			uint32_t column = position % TERMINAL_COLUMNS;

			if (row >= TERMINAL_ROWS) {
				break;
			}

			if (index < g_length) {
				terminal_glyph(16 + column * 8, 16 + row * 16, g_line[index], 0xffa7e3b0);
			}

			if (index == g_cursor) {
				for (uint32_t x = 0; x < 8; x++) {
					g_pixels[(16 + row * 16 + 15) * TERMINAL_WIDTH + 16 + column * 8 + x] = 0xffa7e3b0;
				}
			}
		}
	}

	nxu_ui_submit_t submit = {.pixels = (uint64_t)(uintptr_t)g_pixels,
							  .width = TERMINAL_WIDTH,
							  .height = TERMINAL_HEIGHT,
							  .stride = TERMINAL_WIDTH};
	(void)nxu_ui_submit(connection, &submit);
	g_dirty = false;
}

static void
terminal_recall(bool previous)
{
	if (g_history_count == 0) {
		return;
	}

	if (g_history_position == g_history_count) {
		memcpy(g_saved_line, g_line, sizeof(g_line));
	}

	if (previous && g_history_position > 0) {
		g_history_position--;
	} else if (!previous && g_history_position < g_history_count) {
		g_history_position++;
	}

	memcpy(g_line,
		   g_history_position == g_history_count ? g_saved_line
												 : g_history[g_history_position % TERMINAL_COMMAND_HISTORY],
		   sizeof(g_line));
	g_length = nxu_strlen(g_line);
	g_cursor = g_length;
}

static void
terminal_complete(void)
{
	/* Complete the executable name from the installed CoreServices directory. */
	for (uint32_t index = 0; index < g_length; index++) {
		if (g_line[index] == ' ') {
			return;
		}
	}

	int64_t descriptor = nxu_open("/disk/System/Library/CoreServices", NXU_O_READ);

	if (descriptor < 0) {
		return;
	}

	nxu_dirent_t entry;
	char match[NZH_LINE_MAX] = {0};
	uint32_t matches = 0;

	while (nxu_readdir(descriptor, &entry) > 0) {
		uint32_t index = 0;

		while (index < g_length && entry.name[index] == g_line[index]) {
			index++;
		}

		if (index == g_length && entry.name[0] != '.' && nxu_strlen(entry.name) + 2 < sizeof(match)) {
			memcpy(match, entry.name, nxu_strlen(entry.name) + 1);
			matches++;
		}
	}

	(void)nxu_close(descriptor);

	if (matches == 1) {
		g_length = nxu_strlen(match);
		memcpy(g_line, match, g_length);
		g_line[g_length++] = ' ';
		g_line[g_length] = 0;
		g_cursor = g_length;
	}
}

static bool
terminal_key(const nxu_ui_message_t *message, uint32_t stream, uint32_t shell_pid)
{
	uint32_t key = message->button;
	uint32_t character = message->character;

	if (character == 3) {
#if defined(__i386__)
		char interrupt = 3;
		(void)nxu_write(stream, &interrupt, 1);
		(void)shell_pid;
#else
		(void)nxu_kill(shell_pid, NXU_SIGINT);
#endif
		g_length = 0;
		g_cursor = 0;
		g_line[0] = 0;
		terminal_text("^C\n");
	} else if (key == 103 || key == 108) {
		terminal_recall(key == 103);
	} else if (key == 105 && g_cursor > 0) {
		g_cursor--;
	} else if (key == 106 && g_cursor < g_length) {
		g_cursor++;
	} else if (key == 102) {
		g_cursor = 0;
	} else if (key == 107) {
		g_cursor = g_length;
	} else if ((key == 14 || character == 8 || character == 127) && g_cursor > 0) {
		memmove(g_line + g_cursor - 1, g_line + g_cursor, g_length - g_cursor + 1);
		g_cursor--;
		g_length--;
	} else if (key == 111 && g_cursor < g_length) {
		memmove(g_line + g_cursor, g_line + g_cursor + 1, g_length - g_cursor);
		g_length--;
	} else if (key == 15 || character == '\t') {
		terminal_complete();
	} else if (key == 28 || character == '\n' || character == '\r') {
		g_line[g_length] = 0;
		terminal_text(g_line);
		terminal_append('\n');

		if (g_length != 0) {
			if (g_history_count == TERMINAL_COMMAND_HISTORY) {
				memmove(g_history[0], g_history[1], sizeof(g_history) - sizeof(g_history[0]));
				g_history_count--;
			}
			memcpy(g_history[g_history_count++], g_line, g_length + 1);
		}

		g_history_position = g_history_count;
		g_line[g_length++] = '\n';
		if (nxu_write(stream, g_line, g_length) != g_length) {
			return false;
		}

		g_length = 0;
		g_cursor = 0;
		g_line[0] = 0;
	} else if (character >= 32 && character < 127 && g_length + 2 < sizeof(g_line)) {
		memmove(g_line + g_cursor + 1, g_line + g_cursor, g_length - g_cursor + 1);
		g_line[g_cursor++] = character;
		g_length++;
	}

	g_scroll = 0;
	g_dirty = true;
	return true;
}

int
main(void)
{
	memset(g_rows, ' ', sizeof(g_rows));
	char name[64] = "com.nxu.terminal.";
	uint32_t length = nxu_strlen(name);
	uint32_t pid = nxu_getpid();

	/* A hexadecimal PID makes a bounded, unique local rendezvous name. */
	for (int32_t shift = 28; shift >= 0; shift -= 4) {
		name[length++] = "0123456789abcdef"[(pid >> shift) & 15];
	}
	name[length] = 0;

#if defined(__i386__)
	const char *arguments[] = {"nzh", 0};
	int64_t stream = -1;
	int64_t shell_pid = nxu_spawn_io("/disk/System/Library/CoreServices/nzh", arguments, &stream);

	if (shell_pid <= 0) {
		return 4;
	}
#else
	int64_t listener = nxu_socket_listen(name, 1);

	if (listener < 0) {
		return 1;
	}

	int64_t shell_pid = nxu_fork();

	if (shell_pid == 0) {
		(void)nxu_close(listener);
		int64_t stream = nxu_socket_connect(name);

		if (stream < 0 || nxu_dup2(stream, 0) < 0 || nxu_dup2(stream, 1) < 0 || nxu_dup2(stream, 2) < 0) {
			return 2;
		}

		(void)nxu_close(stream);
		(void)nxu_exec("/disk/System/Library/CoreServices/nzh", 0);
		return 3;
	}

	if (shell_pid < 0) {
		(void)nxu_close(listener);
		return 4;
	}

	int64_t stream = nxu_socket_accept(listener);
	(void)nxu_close(listener);

	if (stream < 0) {
		(void)nxu_kill(shell_pid, NXU_SIGTERM);
		return 5;
	}

#endif

	nxu_ui_session_t session;

	while (nxu_ui_control(NXU_UI_CONTROL_SESSION, (uint64_t)(uintptr_t)&session) < 0) {
		(void)nxu_sleep_us(10000);
	}

	nxu_ui_connect_t info = {.struct_size = sizeof(info),
							 .kind = NXU_UI_KIND_APP,
							 .width = TERMINAL_WIDTH,
							 .height = TERMINAL_HEIGHT + 24,
							 .min_width = TERMINAL_WIDTH,
							 .min_height = TERMINAL_HEIGHT + 24,
							 .max_width = TERMINAL_WIDTH,
							 .max_height = TERMINAL_HEIGHT + 24,
							 .titlebar_height = 24,
							 .corner_radius = 24};
	memcpy(info.name, "Terminal", sizeof("Terminal"));
	memcpy(info.bundle_path, "/Applications/Terminal.app", sizeof("/Applications/Terminal.app"));
	int64_t connection = nxu_ui_connect(&info);

	if (connection <= 0) {
		(void)nxu_close(stream);
		(void)nxu_kill(shell_pid, NXU_SIGTERM);
		return 6;
	}

	bool running = true;

	while (running) {
		char output[512];

		for (uint32_t budget = 0; budget < 16; budget++) {
			int64_t count = nxu_read_poll(stream, output, sizeof(output));

			if (count == -NXU_SYS_E_AGAIN) {
				break;
			}

			if (count <= 0) {
				running = false;
				break;
			}

			for (int64_t index = 0; index < count; index++) {
				terminal_append(output[index]);
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
		} else if (message.type == NXU_UI_MSG_EVENT && message.event == NXU_UI_EVENT_KEY_DOWN) {
			running = terminal_key(&message, stream, shell_pid);
		} else if (message.type == NXU_UI_MSG_EVENT && message.event == NXU_UI_EVENT_SCROLL) {
			int32_t scroll = (int32_t)g_scroll + (message.delta > 0 ? 3 : -3);
			uint32_t limit =
				g_row < TERMINAL_HISTORY_ROWS - TERMINAL_ROWS ? g_row : TERMINAL_HISTORY_ROWS - TERMINAL_ROWS;
			g_scroll = scroll < 0 ? 0 : (uint32_t)scroll > limit ? limit : (uint32_t)scroll;
			g_dirty = true;
		}
	}

	(void)nxu_close(stream);
	(void)nxu_kill(shell_pid, NXU_SIGTERM);
	uint64_t status;
	(void)nxu_wait(shell_pid, &status);
	return 0;
}
