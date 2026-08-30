#ifndef NXU_VFS_FILE_H
#define NXU_VFS_FILE_H

#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>

#define VFS_FD_MAX 32U
#define VFS_FD_FIRST_FILE 3U
#define VFS_FILE_MAX 128U

#define VFS_OPEN_READ (1U << 0U)
#define VFS_OPEN_WRITE (1U << 1U)
#define VFS_OPEN_CREATE (1U << 2U)
#define VFS_OPEN_TRUNCATE (1U << 3U)
#define VFS_OPEN_APPEND (1U << 4U)

struct file;
typedef struct file *file_t;

typedef struct filedesc *filedesc_t;

/*
 * struct file
 *
 * One open instance of a vnode.
 *
 * The file object owns one vnode reference and one seek offset. Descriptor
 * tables refer to file objects rather than vnodes directly so multiple opens
 * of the same vnode do not share an offset accidentally.
 */
struct file {
	vnode_t f_vnode;
	uint64_t f_offset;
	uint32_t f_flags;
	uint32_t f_refcount;
	uint32_t f_slot;
	bool f_active;
};

/*
 * struct filedesc
 *
 * Per-process descriptor table.
 *
 * Descriptor numbers are indexes into fd_ofiles. The lowest free descriptor
 * is chosen for a new open. A filedesc owns one reference to every installed
 * file object and releases those references during process exit.
 */
struct filedesc {
	file_t fd_ofiles[VFS_FD_MAX];
	uint32_t fd_open_count;
	uint32_t fd_freefile;
	volatile uint32_t fd_lock;
};

bool file_table_init(void);

void filedesc_init(filedesc_t filedesc);
void filedesc_close_all(filedesc_t filedesc);

vfs_status_t file_alloc(vnode_t vnode, uint32_t flags, file_t *result);
bool file_reference(file_t file);
void file_rele(file_t file);

vfs_status_t filedesc_install(filedesc_t filedesc, file_t file, uint32_t *descriptor);
vfs_status_t filedesc_get(filedesc_t filedesc, uint32_t descriptor, file_t *result);
vfs_status_t filedesc_remove(filedesc_t filedesc, uint32_t descriptor, file_t *result);

vfs_status_t file_read(file_t file, void *buffer, uint64_t size, uint64_t *read_size);
vfs_status_t file_readdir(file_t file, vfs_dirent_t *entry);
vfs_status_t file_write(file_t file, const void *buffer, uint64_t size, uint64_t *written_size);
vfs_status_t file_seek(file_t file, uint64_t offset);

#endif
