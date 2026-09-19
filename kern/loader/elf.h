#ifndef NXU_KERN_EXEC_ELF_H
#define NXU_KERN_EXEC_ELF_H

#include <kern/process/proc.h>

#include <stdbool.h>

typedef enum {
	EXEC_STATUS_OK = 0,
	EXEC_STATUS_INVALID,
	EXEC_STATUS_NOT_FOUND,
	EXEC_STATUS_BAD_FORMAT,
	EXEC_STATUS_NO_MEMORY,
	EXEC_STATUS_IO_ERROR,
	EXEC_STATUS_PROCESS_ERROR
} exec_status_t;

/* Load a static AArch64 ELF image from VFS and start it as a child process. */
exec_status_t exec_spawn(proc_t parent, const char *path, const char *name, proc_t *result);

/* Compare two regular-file images byte-for-byte. */
exec_status_t exec_images_equal(const char *left_path, const char *right_path, bool *equal);

const char *exec_status_name(exec_status_t status);

#endif
