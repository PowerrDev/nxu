#ifndef NXU_KERN_LOADER_ELF_H
#define NXU_KERN_LOADER_ELF_H

#include <kern/process/proc.h>

#include <stdbool.h>

typedef enum {
	LOADER_STATUS_OK = 0,
	LOADER_STATUS_INVALID,
	LOADER_STATUS_NOT_FOUND,
	LOADER_STATUS_BAD_FORMAT,
	LOADER_STATUS_NO_MEMORY,
	LOADER_STATUS_IO_ERROR,
	LOADER_STATUS_PROCESS_ERROR
} loader_status_t;

/*
 * Load a static executable from VFS and start it as a child process. The
 * accepted format follows the build target: ELF64/EM_AARCH64 on arm64,
 * ELF32/EM_386 on i386 (see kern/loader/elf_format.h).
 */
loader_status_t loader_spawn(proc_t parent, const char *path, const char *name, proc_t *result);

/* Compare two regular-file images byte-for-byte. */
loader_status_t loader_images_equal(const char *left_path, const char *right_path, bool *equal);

const char *loader_status_name(loader_status_t status);

#endif
