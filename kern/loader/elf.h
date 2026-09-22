#ifndef NXU_KERN_LOADER_ELF_H
#define NXU_KERN_LOADER_ELF_H

#include <kern/process/proc.h>
#include <kern/syscall/syscall_defs.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	LOADER_STATUS_OK = 0,
	LOADER_STATUS_INVALID,
	LOADER_STATUS_NOT_FOUND,
	LOADER_STATUS_BAD_FORMAT,
	LOADER_STATUS_NO_MEMORY,
	LOADER_STATUS_IO_ERROR,
	LOADER_STATUS_PROCESS_ERROR,
	LOADER_STATUS_BUSY
} loader_status_t;

/*
 * Load a static executable from VFS and start it as a child process. The
 * accepted format follows the build target: ELF64/EM_AARCH64 on arm64,
 * ELF32/EM_386 on i386 (see kern/loader/elf_format.h).
 */
loader_status_t loader_spawn(proc_t parent, const char *path, const char *name, proc_t *result);

/* Leave the child with the set it would get by default (every capability for PID 1, none otherwise). */
#define LOADER_CAPS_DEFAULT 0xFFFFFFFFU

/*
 * As loader_spawn, but the child starts holding `caps` (NXU_CAP_*). They are set
 * before its thread becomes runnable: with several CPUs the child may already
 * be running elsewhere by the time this returns, so setting them afterwards
 * (proc_set_caps) would let it see an empty set.
 */
loader_status_t loader_spawn_caps(proc_t parent, const char *path, const char *name, uint32_t caps, proc_t *result);

/*
 * argv for loader_exec: argc NUL-terminated strings packed back to back in
 * strings, length bytes in all (terminators included).
 */
typedef struct {
	uint32_t argc;
	uint32_t length;
	char strings[NXU_EXEC_ARGV_BYTES];
} loader_exec_args_t;

/*
 * Replace the running program of proc, in place, with the static executable
 * at path (the exec system call). The new image is built in a separate
 * address space first, so a failure of any kind leaves the caller's program
 * untouched and returns an error; success swaps it in, discards the old one
 * and rewrites the calling thread's user registers to enter the new program
 * with argc in x0 and argv in x1.
 *
 * Open descriptors and port names carry over; caught signals revert to the
 * default. Refused with LOADER_STATUS_BUSY while proc has more than one
 * thread. Must be called by proc's own thread from a system call, and is
 * only implemented where the loader format is native (not on i386).
 */
loader_status_t loader_exec(proc_t proc, const char *path, const loader_exec_args_t *args);

/* Compare two regular-file images byte-for-byte. */
loader_status_t loader_images_equal(const char *left_path, const char *right_path, bool *equal);

const char *loader_status_name(loader_status_t status);

#endif
