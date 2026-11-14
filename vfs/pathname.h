/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/pathname.h
 *
 * Turning the path a program gives into the absolute, canonical path the
 * VFS looks up: a relative path is taken from the process's working
 * directory, and ".", "..", repeated and trailing slashes are worked out.
 *
 * ".." is resolved by name, removing the component before it ("/" stays
 * "/"). That is exactly what the lookup would do, because the lookup
 * (vfs/path.c) walks names from the root of the longest matching mount and
 * follows no symbolic links: "/Volumes/mainOS/.." is "/Volumes" whatever is
 * mounted where.
 *
 * Pure code: no locks, no allocation, no kernel headers, so it is tested on
 * the host (tools/vfs).
 */

#ifndef NXU_VFS_PATHNAME_H
#define NXU_VFS_PATHNAME_H

#include <stdbool.h>
#include <stdint.h>

/*
 * vfs_pathname_resolve
 *
 * `path` made absolute against `base` (an absolute path, normally the
 * working directory; ignored when `path` is absolute) and canonical: it
 * starts with "/", has no "." or ".." components, no empty components and no
 * trailing slash (unless it is "/"). Written to `out`, at most `capacity`
 * bytes with the terminator. False for an empty path, a relative path with
 * no absolute base, or a result that does not fit.
 */
bool vfs_pathname_resolve(const char *base, const char *path, char *out, uint32_t capacity);

#endif
