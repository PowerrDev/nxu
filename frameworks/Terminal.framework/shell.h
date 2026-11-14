/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_TERMINAL_SHELL_H
#define NXU_TERMINAL_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#define NZH_LINE_MAX 256U
#define NZH_ARGUMENT_MAX 15U

/* In-place tokenizer: supports single/double quotes and escapes, rejects operators. */
int nzh_parse(char *line, const char *arguments[NZH_ARGUMENT_MAX + 1]);

/*
 * The line editor nzh reads commands with when its input is a terminal
 * (line_edit.c). It puts the terminal in raw mode for the length of one
 * line and restores the settings it found before returning, so the command
 * that runs next gets its terminal exactly as it was.
 */
#define NZH_HISTORY_MAX 32U

typedef struct {
	char history[NZH_HISTORY_MAX][NZH_LINE_MAX];
	uint32_t count;
} nzh_editor_t;

/* What nzh_edit_line returns when there is no line. */
#define NZH_EDIT_EOF (-1)
#define NZH_EDIT_INTERRUPTED (-2)

/*
 * nzh_edit_line
 *
 * Show `prompt` and read one line from descriptor 0 into `line` (NUL-
 * terminated, without the newline), with editing and the history in
 * `editor`. Returns its length; NZH_EDIT_EOF for ^D on an empty line or a
 * terminal that has gone; NZH_EDIT_INTERRUPTED when a signal handler set
 * `*interrupted` while the line was being typed (^C).
 */
int nzh_edit_line(nzh_editor_t *editor, const char *prompt, char *line, uint32_t capacity,
				  volatile bool *interrupted);

#endif
