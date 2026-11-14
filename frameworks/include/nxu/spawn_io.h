/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_USER_SPAWN_IO_H
#define NXU_USER_SPAWN_IO_H

#include <kern/syscall/termios_defs.h>
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

/*
 * nxu_spawn_pty
 *
 * nxu_spawn_io on a pseudo-terminal instead of a socket, which is how a
 * terminal emulator starts a shell. A new pair is made; the child leads a new
 * session whose controlling terminal is the slave, open as its standard
 * input, output and error, with its group in the foreground. The master is
 * returned in *master: what the child writes, after output processing, is
 * read from it, and what is written to it is typed at the child's terminal,
 * through its line discipline (echo, line editing, ^C as SIGINT). `size`, if
 * not null, is the terminal's window size from the start. Returns the child's
 * pid, or a negative error.
 */
int64_t nxu_spawn_pty(const char *path, const char *const *arguments, const struct nxu_winsize *size, int64_t *master);

/*
 * nxu_spawn_here
 *
 * Start the program at path with the caller's standard input, output and
 * error and the caller's capabilities, in the caller's process group: what
 * fork and exec would give it, for a program (su, sudo) that runs another on
 * its own terminal and waits for it. Returns the child's pid, or a negative
 * error.
 */
int64_t nxu_spawn_here(const char *path, const char *const *arguments);

/*
 * nxu_wait_as_own
 *
 * Wait for `child` and end the caller the way it ended: the same exit status,
 * or the same signal (sent to the caller with its default action restored).
 * For su and sudo, whose own end should be what they ran. Returns only if the
 * wait fails, with -1.
 */
int nxu_wait_as_own(int64_t child);

#endif
