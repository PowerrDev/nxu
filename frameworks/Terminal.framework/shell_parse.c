/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include "shell.h"

int
nzh_parse(char *line, const char *arguments[NZH_ARGUMENT_MAX + 1])
{
	char *read = line;
	char *write = line;
	uint32_t count = 0;

	while (*read != 0) {
		while (*read == ' ' || *read == '\t') {
			read++;
		}

		if (*read == 0) {
			break;
		}

		if (count == NZH_ARGUMENT_MAX) {
			return -1;
		}

		arguments[count++] = write;
		char quote = 0;

		while (*read != 0 && (quote != 0 || (*read != ' ' && *read != '\t'))) {
			char character = *read++;

			if (character == '\\' && quote != '\'') {
				if (*read == 0) {
					return -1;
				}

				*write++ = *read++;
			} else if (character == '\'' || character == '"') {
				if (quote == character) {
					quote = 0;
				} else if (quote == 0) {
					quote = character;
				} else {
					*write++ = character;
				}
			} else if (quote == 0 && (character == '|' || character == '&' || character == '<' || character == '>')) {
				return -1;
			} else {
				*write++ = character;
			}
		}

		if (quote != 0) {
			return -1;
		}

		/* Advance past the separator before compacting the terminating NUL. */
		if (*read != 0) {
			read++;
		}

		*write++ = 0;
	}

	arguments[count] = 0;
	return count;
}
