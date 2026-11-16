/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/loader/elf.h
 *
 * Declares the kernel loader elf interfaces and data structures.
 */
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
	LOADER_STATUS_BUSY,
	LOADER_STATUS_DENIED
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
 * As loader_spawn_caps, but the child starts as user `uid` and group `gid`
 * (real, effective and saved; no supplementary groups) instead of with its
 * parent's credentials. Like the capabilities, they are in place before its
 * thread becomes runnable. For the kernel starting a process that must not be
 * root: a child of the kernel process would otherwise inherit root, and root
 * holds every capability whatever `caps` says. A setuid or setgid executable
 * still takes its file's identity.
 */
loader_status_t loader_spawn_as(proc_t parent, const char *path, const char *name, uint32_t caps, uint32_t uid,
								uint32_t gid, proc_t *result);

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
 * loader_spawn_request_t
 *
 * Everything a process is to start with. loader_spawn_request applies all of
 * it while the process is still an embryo -- created and visible, but with no
 * thread that can run -- and only then starts it. If any part cannot be
 * applied the embryo is taken apart again and no process results, so a
 * process is never seen running half configured.
 *
 * name
 *     The process name.
 *
 * caps
 *     Its capabilities, or LOADER_CAPS_DEFAULT.
 *
 * args
 *     Its arguments, or 0 for none (argc 0).
 *
 * group
 *     Its process group and session (kern/process/proc.h). Applied inside
 *     proc_create_user_in, in the critical section that makes it visible.
 *
 * set_sigmask / sigmask
 *     The signal mask its first thread starts with.
 *
 * descriptors / descriptor_count
 *     Its descriptors: each `child` number refers, in the new process, to the
 *     open file the `parent` number refers to in the parent.
 *
 * set_identity / uid / gid
 *     Start it as this user and group instead of with the parent's
 *     credentials. For kernel callers; the spawn system call never sets it.
 *
 * set_ctty / ctty
 *     The terminal at its descriptor `ctty` becomes the controlling terminal
 *     of the new session it leads (group.mode PROC_GROUP_NEW_SESSION).
 *
 * set_foreground
 *     Its process group becomes the foreground group of the parent's
 *     controlling terminal.
 */
typedef struct {
	const char *name;
	uint32_t caps;
	const loader_exec_args_t *args;
	proc_group_t group;
	bool set_sigmask;
	uint32_t sigmask;
	uint32_t descriptor_count;
	nxu_spawn_fd_t descriptors[NXU_SPAWN_FD_MAX];
	bool set_identity;
	uint32_t uid;
	uint32_t gid;
	bool set_ctty;
	uint32_t ctty;
	bool set_foreground;
} loader_spawn_request_t;

/*
 * Load the executable at path and start it as a child of parent, configured
 * as `request` says. LOADER_STATUS_DENIED: the parent may not execute the file,
 * or the process group to join does not exist in the parent's session.
 * LOADER_STATUS_INVALID: a descriptor named in
 * the request is not open in the parent, or is named twice for the child.
 */
loader_status_t loader_spawn_request(proc_t parent, const char *path, const loader_spawn_request_t *request,
									 proc_t *result);

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
/* ARM64 PIE image mapping for the in-process runtime. */
loader_status_t loader_dyrld_map(proc_t proc, nxu_dyrld_image_t *image);
loader_status_t loader_dyrld_finalize(proc_t proc);

#endif
