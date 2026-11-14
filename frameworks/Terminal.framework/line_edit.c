/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/Terminal.framework/line_edit.c
 *
 * nzh's line editor: what a shell does on a real terminal, where the
 * emulator only shows what it is sent and types what it is given.
 *
 * For one line the terminal is in raw mode (no echo, no canonical input),
 * so every key arrives as it is pressed and the editor does the echo itself
 * by redrawing the line. Signals stay on: ^C is still SIGINT from the line
 * discipline, which ends the read with "interrupted" (nzh's handler does not
 * ask for the call to be restarted) and so abandons the line. Before
 * returning, the editor puts back the settings it found, so a command is
 * never started in raw mode.
 *
 * Keys, as the emulator sends them:
 *
 *   printable           insert at the cursor
 *   Return              finish the line
 *   Backspace (DEL, ^H) delete before the cursor
 *   Delete (ESC [ 3 ~)  delete under the cursor
 *   Left/Right, ^B/^F   move
 *   Home/End, ^A/^E     to the start or end
 *   Up/Down, ^P/^N      history
 *   ^U / ^K / ^W        delete to the start / to the end / the word before
 *   ^L                  clear the screen
 *   ^D                  end of file on an empty line, else delete
 *   Tab                 complete the command name
 *
 * A line longer than the terminal is wide wraps onto the rows below. The
 * redraw goes back up to the row the prompt is on, writes prompt and line,
 * erases what is below, and moves to the cursor; for that it needs of the
 * emulator carriage return, ESC [ n A (up), ESC [ n C (right) and ESC [ J
 * (erase to the end of the screen), and to wrap the way terminals do, only
 * when a character is written past the last column. The width is the
 * terminal's (TIOCGWINSZ), 80 when it has none.
 *
 * ESC is both a key and the start of what the cursor keys send. The rest of
 * a sequence comes in the same write, so after an ESC the editor waits a
 * tenth of a second (VTIME) for it; nothing means Escape alone, which is
 * ignored.
 */
#include "shell.h"

#include <nxu/string.h>
#include <nxu/syscall.h>
#include <termios.h>

#define EDIT_COMMANDS_DIRECTORY "/Volumes/mainOS/System/Library/CoreServices"

typedef struct {
	nzh_editor_t *editor;
	const char *prompt;
	char *line;
	uint32_t capacity;
	uint32_t length;
	uint32_t cursor;

	/* Where Up and Down are in the history; `count` is the line being typed, kept in `saved`. */
	uint32_t position;
	char saved[NZH_LINE_MAX];

	/* The terminal's width, the prompt's length, and the row (from the prompt's) the cursor was left on. */
	uint32_t columns;
	uint32_t prompt_length;
	uint32_t cursor_row;

	/* The raw settings the line is read under, for the timed read after an ESC. */
	struct termios raw;
} edit_state_t;

static void
edit_write(const char *text, uint64_t length)
{
	if (length != 0) (void)nxu_write(1, text, length);
}

static void
edit_puts(const char *text)
{
	edit_write(text, nxu_strlen(text));
}

/* ESC [ n letter. */
static void
edit_csi(uint32_t count, char letter)
{
	char sequence[16];
	char digits[10];
	uint32_t length = 0;
	uint32_t used = 0;

	do {
		digits[used++] = (char)('0' + count % 10U);
		count /= 10U;
	} while (count != 0 && used < sizeof(digits));

	sequence[length++] = '\033';
	sequence[length++] = '[';

	while (used != 0)
		sequence[length++] = digits[--used];

	sequence[length++] = letter;
	edit_write(sequence, length);
}

/*
 * Where the cursor is after `count` characters from the prompt's start: a
 * terminal wraps only when the next character comes, so after exactly a
 * row's worth it is still past the end of that row.
 */
static uint32_t
edit_row_after(const edit_state_t *state, uint32_t count)
{
	return count != 0 && count % state->columns == 0 ? count / state->columns - 1 : count / state->columns;
}

/* Up to the prompt's row, prompt and line, erase below, then to the cursor. */
static void
edit_redraw(edit_state_t *state)
{
	if (state->cursor_row != 0) edit_csi(state->cursor_row, 'A');

	edit_puts("\r");
	edit_puts(state->prompt);
	edit_write(state->line, state->length);
	edit_puts("\033[J");

	uint32_t end = state->prompt_length + state->length;
	uint32_t end_row = edit_row_after(state, end);

	state->cursor_row = end_row;

	if (state->cursor == state->length) return;

	uint32_t at = state->prompt_length + state->cursor;
	uint32_t row = at / state->columns;
	uint32_t column = at % state->columns;

	if (end_row > row) edit_csi(end_row - row, 'A');

	edit_puts("\r");
	if (column != 0) edit_csi(column, 'C');

	state->cursor_row = row;
}

static void
edit_replace(edit_state_t *state, const char *text)
{
	uint32_t length = (uint32_t)nxu_strlen(text);

	if (length >= state->capacity) length = state->capacity - 1;

	memcpy(state->line, text, length);
	state->line[length] = 0;
	state->length = length;
	state->cursor = length;
}

static void
edit_insert(edit_state_t *state, char character)
{
	if (state->length + 1 >= state->capacity) return;

	memmove(state->line + state->cursor + 1, state->line + state->cursor, state->length - state->cursor + 1);
	state->line[state->cursor++] = character;
	state->length++;
}

/* Delete `count` characters starting at `at`. */
static void
edit_delete(edit_state_t *state, uint32_t at, uint32_t count)
{
	if (at >= state->length || count == 0) return;
	if (count > state->length - at) count = state->length - at;

	memmove(state->line + at, state->line + at + count, state->length - at - count + 1);
	state->length -= count;

	if (state->cursor > at + count) {
		state->cursor -= count;
	} else if (state->cursor > at) {
		state->cursor = at;
	}
}

static void
edit_history(edit_state_t *state, bool previous)
{
	nzh_editor_t *editor = state->editor;

	if (previous && state->position == 0) return;
	if (!previous && state->position == editor->count) return;

	/* Leaving the line being typed: keep it, to come back to. */
	if (state->position == editor->count) memcpy(state->saved, state->line, state->length + 1);

	if (previous) {
		state->position--;
	} else {
		state->position++;
	}

	edit_replace(state, state->position == editor->count ? state->saved : editor->history[state->position]);
}

/* Tab: complete the first word from the installed commands when exactly one matches. */
static void
edit_complete(edit_state_t *state)
{
	for (uint32_t index = 0; index < state->length; index++) {
		if (state->line[index] == ' ') return;
	}

	int64_t directory = nxu_open(EDIT_COMMANDS_DIRECTORY, NXU_O_READ);

	if (directory < 0) return;

	nxu_dirent_t entry;
	char match[NZH_LINE_MAX] = {0};
	uint32_t matches = 0;

	while (nxu_readdir(directory, &entry) > 0) {
		uint32_t index = 0;

		while (index < state->length && entry.name[index] == state->line[index])
			index++;

		if (index == state->length && entry.name[0] != '.' && nxu_strlen(entry.name) + 2 < sizeof(match)) {
			memcpy(match, entry.name, nxu_strlen(entry.name) + 1);
			matches++;
		}
	}

	(void)nxu_close(directory);

	if (matches != 1) {
		edit_puts("\a");
		return;
	}

	edit_replace(state, match);
	edit_insert(state, ' ');
}

static void
edit_remember(nzh_editor_t *editor, const char *line, uint32_t length)
{
	if (length == 0) return;

	if (editor->count == NZH_HISTORY_MAX) {
		memmove(editor->history[0], editor->history[1], sizeof(editor->history) - sizeof(editor->history[0]));
		editor->count--;
	}

	memcpy(editor->history[editor->count++], line, length + 1);
}

/* One byte, or none if nothing comes within a tenth of a second. */
static bool
edit_read_soon(edit_state_t *state, char *character)
{
	struct termios timed = state->raw;

	timed.c_cc[VMIN] = 0;
	timed.c_cc[VTIME] = 1;
	(void)tcsetattr(0, TCSANOW, &timed);

	int64_t count = nxu_read(0, character, 1);

	(void)tcsetattr(0, TCSANOW, &state->raw);
	return count == 1;
}

/* One escape sequence after ESC: CSI (ESC [) or SS3 (ESC O), as cursor keys send; Escape alone does nothing. */
static void
edit_escape(edit_state_t *state)
{
	char introducer;
	char final;
	char parameter = 0;

	if (!edit_read_soon(state, &introducer) || (introducer != '[' && introducer != 'O')) return;
	if (!edit_read_soon(state, &final)) return;

	/* ESC [ 3 ~ and its like: one digit, then the tilde. */
	if (final >= '0' && final <= '9') {
		parameter = final;
		if (!edit_read_soon(state, &final) || final != '~') return;
	}

	switch (parameter != 0 ? parameter : final) {
	case 'A':
		edit_history(state, true);
		break;
	case 'B':
		edit_history(state, false);
		break;
	case 'C':
		if (state->cursor < state->length) state->cursor++;
		break;
	case 'D':
		if (state->cursor > 0) state->cursor--;
		break;
	case 'H':
	case '1':
	case '7':
		state->cursor = 0;
		break;
	case 'F':
	case '4':
	case '8':
		state->cursor = state->length;
		break;
	case '3':
		edit_delete(state, state->cursor, 1);
		break;
	default:
		break;
	}
}

/* Not a terminal after all: read a line the plain way, one byte at a time. */
static int
edit_line_plain(const char *prompt, char *line, uint32_t capacity)
{
	uint32_t length = 0;

	edit_puts(prompt);

	for (;;) {
		char character;
		int64_t result = nxu_read(0, &character, 1);

		if (result <= 0) return length == 0 ? NZH_EDIT_EOF : (int)length;
		if (character == '\n') break;
		if (length + 1 < capacity) line[length++] = character;
	}

	line[length] = 0;
	return (int)length;
}

int
nzh_edit_line(nzh_editor_t *editor, const char *prompt, char *line, uint32_t capacity, volatile bool *interrupted)
{
	struct termios saved;

	if (capacity == 0) return NZH_EDIT_EOF;
	if (tcgetattr(0, &saved) != 0) return edit_line_plain(prompt, line, capacity);

	struct termios raw = saved;

	raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHONL | IEXTEN);
	raw.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	(void)tcsetattr(0, TCSANOW, &raw);

	edit_state_t state = {.editor = editor, .prompt = prompt, .line = line, .capacity = capacity, .raw = raw};
	struct winsize size;
	int result = 0;

	state.columns = tcgetwinsize(0, &size) == 0 && size.ws_col != 0 ? size.ws_col : 80;
	state.prompt_length = (uint32_t)nxu_strlen(prompt);
	state.cursor_row = edit_row_after(&state, state.prompt_length);
	line[0] = 0;
	state.position = editor->count;
	*interrupted = false;
	edit_puts(prompt);

	for (bool done = false; !done;) {
		char character;
		int64_t count = nxu_read(0, &character, 1);

		if (count == -NXU_SYS_E_INTERRUPTED) {
			if (!*interrupted) continue;

			edit_puts("^C\r\n");
			result = NZH_EDIT_INTERRUPTED;
			break;
		}

		if (count != 1) {
			result = NZH_EDIT_EOF;
			break;
		}

		switch ((uint8_t)character) {
		case '\r':
		case '\n':
			/* The newline goes after the whole line, wherever the cursor was in it. */
			if (state.cursor != state.length) {
				state.cursor = state.length;
				edit_redraw(&state);
			}

			edit_puts("\r\n");
			done = true;
			continue;

		case 0x7F:
		case '\b':
			if (state.cursor > 0) edit_delete(&state, state.cursor - 1, 1);
			break;

		case NXU_CTRL('D'):
			if (state.length == 0) {
				edit_puts("\r\n");
				result = NZH_EDIT_EOF;
				done = true;
				continue;
			}

			edit_delete(&state, state.cursor, 1);
			break;

		case NXU_CTRL('A'):
			state.cursor = 0;
			break;

		case NXU_CTRL('E'):
			state.cursor = state.length;
			break;

		case NXU_CTRL('B'):
			if (state.cursor > 0) state.cursor--;
			break;

		case NXU_CTRL('F'):
			if (state.cursor < state.length) state.cursor++;
			break;

		case NXU_CTRL('P'):
			edit_history(&state, true);
			break;

		case NXU_CTRL('N'):
			edit_history(&state, false);
			break;

		case NXU_CTRL('U'):
			edit_delete(&state, 0, state.cursor);
			break;

		case NXU_CTRL('K'):
			edit_delete(&state, state.cursor, state.length - state.cursor);
			break;

		case NXU_CTRL('W'): {
			uint32_t start = state.cursor;

			while (start > 0 && state.line[start - 1] == ' ')
				start--;
			while (start > 0 && state.line[start - 1] != ' ')
				start--;

			edit_delete(&state, start, state.cursor - start);
			break;
		}

		case NXU_CTRL('L'):
			edit_puts("\033[2J\033[H");
			state.cursor_row = 0;
			break;

		case '\t':
			edit_complete(&state);
			break;

		case 0x1B:
			edit_escape(&state);
			break;

		default:
			if ((uint8_t)character >= 32 && (uint8_t)character < 127) edit_insert(&state, character);

			break;
		}

		edit_redraw(&state);
	}

	(void)tcsetattr(0, TCSANOW, &saved);

	if (result != 0) return result;

	edit_remember(editor, line, state.length);
	return (int)state.length;
}
