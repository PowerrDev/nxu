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

/* Load a static AArch64 ELF image from VFS and start it as a child process. */
loader_status_t exec_spawn(proc_t parent, const char *path, const char *name, proc_t *result);

/* Compare two regular-file images byte-for-byte. */
loader_status_t exec_images_equal(const char *left_path, const char *right_path, bool *equal);

const char *exec_status_name(loader_status_t status);

#endif
