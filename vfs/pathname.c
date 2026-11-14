/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/pathname.c
 *
 * Canonical absolute paths; see vfs/pathname.h. The result is built in
 * `out` one component at a time: a name is appended after a "/", and ".."
 * cuts `out` back to the "/" before its last component.
 */

#include <vfs/pathname.h>

#include <stdbool.h>
#include <stdint.h>

/* Append the components of `path` (absolute or not) to the canonical path in out[0, *length). */
static bool
vfs_pathname_append(const char *path, char *out, uint32_t capacity, uint32_t *length)
{
	uint32_t cursor = 0U;

	for (;;) {
		while (path[cursor] == '/')
			cursor++;

		if (path[cursor] == '\0') return true;

		uint32_t first = cursor;

		while (path[cursor] != '\0' && path[cursor] != '/')
			cursor++;

		uint32_t size = cursor - first;

		if (size == 1U && path[first] == '.') continue;

		if (size == 2U && path[first] == '.' && path[first + 1U] == '.') {
			/* Back to the "/" before the last component; the root stays the root. */
			while (*length > 1U && out[*length - 1U] != '/')
				(*length)--;
			if (*length > 1U) (*length)--;
			continue;
		}

		uint32_t separator = *length > 1U ? 1U : 0U;

		if (*length + separator + size + 1U > capacity) return false;

		if (separator != 0U) out[(*length)++] = '/';

		for (uint32_t index = 0U; index < size; index++)
			out[(*length)++] = path[first + index];
	}
}

bool
vfs_pathname_resolve(const char *base, const char *path, char *out, uint32_t capacity)
{
	if (path == 0 || out == 0 || capacity < 2U || path[0] == '\0') return false;

	uint32_t length = 1U;

	out[0] = '/';

	if (path[0] != '/') {
		if (base == 0 || base[0] != '/') return false;
		if (!vfs_pathname_append(base, out, capacity, &length)) return false;
	}

	if (!vfs_pathname_append(path, out, capacity, &length)) return false;

	out[length] = '\0';
	return true;
}
