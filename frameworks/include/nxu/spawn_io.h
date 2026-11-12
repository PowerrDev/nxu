/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_USER_SPAWN_IO_H
#define NXU_USER_SPAWN_IO_H

#include <nxu/syscall.h>

#include <stdint.h>

/*
 * nxu_spawn_pack_arguments
 *
 * Pack the NULL-terminated array `arguments` the way nxu_spawn_attr_t wants
 * it: the strings back to back in `buffer`. Stores how many there are and how
 * many bytes they take. False if there are more than NXU_EXEC_ARGV_MAX or
 * they do not fit in `capacity`.
 */
bool nxu_spawn_pack_arguments(const char *const *arguments, char *buffer, uint32_t capacity, uint32_t *argc,
							  uint32_t *size);

/*
 * nxu_spawn_io
 *
 * Start the program at path with `arguments` (argv, NULL-terminated; argv[0]
 * also names the process, so pass a name of fewer than 32 characters, not a
 * path), holding the caller's capabilities, with one end of a new stream
 * socket as its standard input, output and error. The other end is returned
 * in *stream: what the child writes is read from it, what is written to it
 * the child reads. Returns the child's pid, or a negative error.
 *
 * The child has its arguments and its descriptors from its first instruction;
 * nothing is negotiated after it starts.
 */
int64_t nxu_spawn_io(const char *path, const char *const *arguments, int64_t *stream);

#endif
