/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_TERMINAL_SHELL_H
#define NXU_TERMINAL_SHELL_H

#include <stdint.h>

#define NZH_LINE_MAX 256U
#define NZH_ARGUMENT_MAX 15U

/* In-place tokenizer: supports single/double quotes and escapes, rejects operators. */
int nzh_parse(char *line, const char *arguments[NZH_ARGUMENT_MAX + 1]);

#endif
