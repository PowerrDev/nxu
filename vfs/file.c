#include <vfs/file.h>
#include <mach/machine/cpu.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	volatile uint32_t value;
} file_lock_t;

static struct file g_file_slots[VFS_FILE_MAX];
static bool g_file_slot_used[VFS_FILE_MAX];
static file_lock_t g_file_table_lock;
static bool g_file_table_initialized;

static void file_lock(file_lock_t *lock)
{
	while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void file_unlock(file_lock_t *lock)
{
	__atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

static void filedesc_lock(filedesc_t filedesc)
{
	while (__atomic_exchange_n(&filedesc->fd_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void filedesc_unlock(filedesc_t filedesc)
{
	__atomic_store_n(&filedesc->fd_lock, 0U, __ATOMIC_RELEASE);
}

/*
 * file_free_locked:
 *
 * Return an inactive file object to the global file table.
 */
static void file_free_locked(file_t file)
{
	if (file == 0 || file->f_slot >= VFS_FILE_MAX) return;

	if (&g_file_slots[file->f_slot] != file) return;

	g_file_slot_used[file->f_slot] = false;
	memset(file, 0, sizeof(*file));
}

bool file_table_init(void)
{
	file_lock(&g_file_table_lock);

	if (g_file_table_initialized) {
		file_unlock(&g_file_table_lock);
		return true;
	}

	memset(g_file_slots, 0, sizeof(g_file_slots));
	memset(g_file_slot_used, 0, sizeof(g_file_slot_used));
	g_file_table_initialized = true;

	file_unlock(&g_file_table_lock);
	return true;
}

void filedesc_init(filedesc_t filedesc)
{
	if (filedesc == 0) return;

	memset(filedesc, 0, sizeof(*filedesc));
	filedesc->fd_freefile = VFS_FD_FIRST_FILE;
}

vfs_status_t file_alloc(vnode_t vnode, uint32_t flags, file_t *result)
{
	if (result != 0) *result = 0;
	if (!g_file_table_initialized || vnode == 0 || result == 0) return VFS_STATUS_INVALID;

	if ((flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U) return VFS_STATUS_INVALID;

	file_lock(&g_file_table_lock);

	for (uint32_t slot = 0U; slot < VFS_FILE_MAX; slot++) {
		if (g_file_slot_used[slot]) continue;

		if (!vnode_reference(vnode)) {
			file_unlock(&g_file_table_lock);
			return VFS_STATUS_INVALID;
		}

		g_file_slot_used[slot] = true;
		g_file_slots[slot] = (struct file) {
			.f_type = FILE_TYPE_VNODE,
			.f_vnode = vnode,
			.f_offset = 0ULL,
			.f_flags = flags,
			.f_refcount = 1U,
			.f_slot = slot,
			.f_active = true
		};

		*result = &g_file_slots[slot];
		file_unlock(&g_file_table_lock);
		return VFS_STATUS_OK;
	}

	file_unlock(&g_file_table_lock);
	return VFS_STATUS_NO_SPACE;
}

vfs_status_t file_alloc_socket(socket_t socket, file_t *result)
{
	if (result != 0) *result = 0;
	if (!g_file_table_initialized || socket == SOCKET_NULL || result == 0) return VFS_STATUS_INVALID;

	file_lock(&g_file_table_lock);

	for (uint32_t slot = 0U; slot < VFS_FILE_MAX; slot++) {
		if (g_file_slot_used[slot]) continue;

		g_file_slot_used[slot] = true;
		g_file_slots[slot] = (struct file) {
			.f_type = FILE_TYPE_SOCKET,
			.f_socket = socket,
			.f_offset = 0ULL,
			.f_flags = VFS_OPEN_READ | VFS_OPEN_WRITE,
			.f_refcount = 1U,
			.f_slot = slot,
			.f_active = true
		};

		*result = &g_file_slots[slot];
		file_unlock(&g_file_table_lock);
		return VFS_STATUS_OK;
	}

	file_unlock(&g_file_table_lock);
	return VFS_STATUS_NO_SPACE;
}

bool file_reference(file_t file)
{
	if (file == 0 || !file->f_active) return false;

	file_lock(&g_file_table_lock);

	bool valid = file->f_active && file->f_refcount != UINT32_MAX;
	if (valid) file->f_refcount++;

	file_unlock(&g_file_table_lock);
	return valid;
}

void file_rele(file_t file)
{
	if (file == 0) return;

	file_type_t type = FILE_TYPE_VNODE;
	vnode_t vnode = 0;
	socket_t socket = SOCKET_NULL;

	file_lock(&g_file_table_lock);

	if (file->f_active && file->f_refcount != 0U) {
		file->f_refcount--;

		if (file->f_refcount == 0U) {
			type = file->f_type;
			if (type == FILE_TYPE_VNODE) {
				vnode = file->f_vnode;
			} else {
				socket = file->f_socket;
			}
			file->f_active = false;
			file_free_locked(file);
		}
	}

	file_unlock(&g_file_table_lock);

	if (type == FILE_TYPE_VNODE) {
		if (vnode != 0) vnode_rele(vnode);
	} else {
		if (socket != SOCKET_NULL) socket_close(socket);
	}
}

vfs_status_t filedesc_install(filedesc_t filedesc, file_t file, uint32_t *descriptor)
{
	if (descriptor != 0) *descriptor = UINT32_MAX;
	if (filedesc == 0 || file == 0 || descriptor == 0 || !file->f_active) return VFS_STATUS_INVALID;

	filedesc_lock(filedesc);

	uint32_t start = filedesc->fd_freefile;
	if (start < VFS_FD_FIRST_FILE || start >= VFS_FD_MAX) start = VFS_FD_FIRST_FILE;

	for (uint32_t fd = start; fd < VFS_FD_MAX; fd++) {
		if (filedesc->fd_ofiles[fd] != 0) continue;

		filedesc->fd_ofiles[fd] = file;
		filedesc->fd_open_count++;

		uint32_t next = fd + 1U;
		while (next < VFS_FD_MAX && filedesc->fd_ofiles[next] != 0) next++;
		filedesc->fd_freefile = next;

		*descriptor = fd;
		filedesc_unlock(filedesc);
		return VFS_STATUS_OK;
	}

	filedesc_unlock(filedesc);
	return VFS_STATUS_NO_SPACE;
}

vfs_status_t filedesc_get(filedesc_t filedesc, uint32_t descriptor, file_t *result)
{
	if (result != 0) *result = 0;
	if (filedesc == 0 || result == 0 || descriptor >= VFS_FD_MAX) return VFS_STATUS_BAD_FD;

	filedesc_lock(filedesc);

	file_t file = filedesc->fd_ofiles[descriptor];
	if (file == 0 || !file_reference(file)) {
		filedesc_unlock(filedesc);
		return VFS_STATUS_BAD_FD;
	}

	*result = file;
	filedesc_unlock(filedesc);
	return VFS_STATUS_OK;
}

vfs_status_t filedesc_remove(filedesc_t filedesc, uint32_t descriptor, file_t *result)
{
	if (result != 0) *result = 0;
	if (filedesc == 0 || result == 0 || descriptor >= VFS_FD_MAX) return VFS_STATUS_BAD_FD;

	filedesc_lock(filedesc);

	file_t file = filedesc->fd_ofiles[descriptor];
	if (file == 0) {
		filedesc_unlock(filedesc);
		return VFS_STATUS_BAD_FD;
	}

	filedesc->fd_ofiles[descriptor] = 0;
	if (filedesc->fd_open_count != 0U) filedesc->fd_open_count--;
	if (descriptor >= VFS_FD_FIRST_FILE && descriptor < filedesc->fd_freefile) filedesc->fd_freefile = descriptor;

	*result = file;
	filedesc_unlock(filedesc);
	return VFS_STATUS_OK;
}

void filedesc_close_all(filedesc_t filedesc)
{
	if (filedesc == 0) return;

	for (uint32_t fd = 0U; fd < VFS_FD_MAX; fd++) {
		file_t file;
		if (filedesc_remove(filedesc, fd, &file) == VFS_STATUS_OK) file_rele(file);
	}
}

vfs_status_t file_read(file_t file, void *buffer, uint64_t size, uint64_t *read_size)
{
	if (read_size != 0) *read_size = 0ULL;
	if (file == 0 || buffer == 0 || read_size == 0 || !file->f_active) return VFS_STATUS_INVALID;

	if ((file->f_flags & VFS_OPEN_READ) == 0U) return VFS_STATUS_INVALID;

	if (file->f_type == FILE_TYPE_SOCKET) {
		int64_t result = socket_read(file->f_socket, buffer, size);
		if (result < 0) return VFS_STATUS_IO_ERROR;
		*read_size = (uint64_t)result;
		return VFS_STATUS_OK;
	}

	vfs_status_t status = vnode_read(file->f_vnode, file->f_offset, buffer, size, read_size);
	if (status == VFS_STATUS_OK) file->f_offset += *read_size;
	return status;
}

/*
 * Routine:     file_readdir
 * Purpose:
 *              Read one entry from an open directory and retain the
 *              filesystem enumeration cookie in the file object.
 */
vfs_status_t
file_readdir(file_t file, vfs_dirent_t *entry)
{
	if (file == 0 || entry == 0 || !file->f_active) return VFS_STATUS_INVALID;
	if (file->f_type == FILE_TYPE_SOCKET) return VFS_STATUS_NOT_SUPPORTED;
	if ((file->f_flags & VFS_OPEN_READ) == 0U) return VFS_STATUS_INVALID;

	return vnode_readdir(file->f_vnode, &file->f_offset, entry);
}

vfs_status_t file_write(file_t file, const void *buffer, uint64_t size, uint64_t *written_size)
{
	if (written_size != 0) *written_size = 0ULL;
	if (file == 0 || buffer == 0 || written_size == 0 || !file->f_active) return VFS_STATUS_INVALID;

	if ((file->f_flags & VFS_OPEN_WRITE) == 0U) return VFS_STATUS_INVALID;

	if (file->f_type == FILE_TYPE_SOCKET) {
		int64_t result = socket_write(file->f_socket, buffer, size);
		if (result < 0) return VFS_STATUS_IO_ERROR;
		*written_size = (uint64_t)result;
		return VFS_STATUS_OK;
	}

	uint64_t offset = (file->f_flags & VFS_OPEN_APPEND) != 0U
		? file->f_vnode->v_size
		: file->f_offset;

	vfs_status_t status = vnode_write(file->f_vnode, offset, buffer, size, written_size);
	if (status == VFS_STATUS_OK) file->f_offset = offset + *written_size;
	return status;
}

vfs_status_t file_seek(file_t file, uint64_t offset)
{
	if (file == 0 || !file->f_active) return VFS_STATUS_INVALID;
	if (file->f_type == FILE_TYPE_SOCKET) return VFS_STATUS_NOT_SUPPORTED;
	file->f_offset = offset;
	return VFS_STATUS_OK;
}
