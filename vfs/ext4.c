#include <kern/console/console.h>
#include <kern/machine/cpu.h>
#include <vfs/ext4.h>

#include <drivers/block/block_device.h>
#include <crc32c.h>
#include <kern/memory/heap.h>
#include <platform/uart.h>
#include <vfs/jbd2.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EXT4_SUPERBLOCK_OFFSET 1024ULL
#define EXT4_SUPERBLOCK_SIZE 1024U
#define EXT4_SUPER_MAGIC 0xEF53U
#define EXT4_ROOT_INODE 2U

#define EXT4_MIN_BLOCK_SIZE 1024U
#define EXT4_MAX_BLOCK_SIZE 4096U
#define EXT4_GOOD_OLD_INODE_SIZE 128U

#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x00000004U
#define EXT4_FEATURE_COMPAT_DIR_INDEX 0x00000020U

#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x00000002U
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x00000004U
#define EXT4_FEATURE_INCOMPAT_META_BG 0x00000010U
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x00000040U
#define EXT4_FEATURE_INCOMPAT_64BIT 0x00000080U
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x00000200U
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x00002000U
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x00008000U
#define EXT4_FEATURE_INCOMPAT_ENCRYPT 0x00010000U
#define EXT4_FEATURE_INCOMPAT_CASEFOLD 0x00020000U

#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM 0x00000010U
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC 0x00000200U
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x00000400U
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x00010000U

#define EXT4_SUPPORTED_INCOMPAT ( \
	EXT4_FEATURE_INCOMPAT_FILETYPE | \
	EXT4_FEATURE_INCOMPAT_EXTENTS | \
	EXT4_FEATURE_INCOMPAT_64BIT | \
	EXT4_FEATURE_INCOMPAT_FLEX_BG | \
	EXT4_FEATURE_INCOMPAT_CSUM_SEED | \
	EXT4_FEATURE_INCOMPAT_RECOVER \
)

#define EXT4_EXTENTS_FL 0x00080000U
#define EXT4_INDEX_FL 0x00001000U
#define EXT4_INLINE_DATA_FL 0x10000000U

#define EXT4_EXTENT_MAGIC 0xF30AU
#define EXT4_EXTENT_MAX_DEPTH 5U

#define EXT4_MODE_TYPE_MASK 0xF000U
#define EXT4_MODE_DIRECTORY 0x4000U
#define EXT4_MODE_REGULAR 0x8000U

#define EXT4_MOUNT_MAX VFS_MOUNT_MAX

#define EXT4_BG_INODE_UNINIT 0x0001U
#define EXT4_BG_BLOCK_UNINIT 0x0002U

#define EXT4_DIR_FT_REGULAR 1U
#define EXT4_DIR_FT_DIRECTORY 2U

#define EXT4_INODE_MODE_REGULAR 0x81A4U
#define EXT4_INODE_MODE_DIRECTORY 0x41EDU

#define EXT4_ROOT_EXTENT_CAPACITY 4U

#define EXT4_JOURNAL_INODE_DEFAULT 8U
#define EXT4_CHECKSUM_TYPE_CRC32C 1U
#define EXT4_DIRECTORY_TAIL_SIZE 12U
#define EXT4_DIRECTORY_TAIL_FT 0xDEU

/*
 * ext4_inode
 *
 * Host-endian subset of the on-disk inode needed by filesystem operations. i_block
 * retains the 60-byte extent root exactly as stored on disk.
 */
typedef struct {
	uint32_t number;
	uint16_t mode;
	uint32_t flags;
	uint64_t size;
	uint32_t generation;
	uint16_t links_count;
	uint64_t blocks_512;
	uint8_t block[60];
} ext4_inode_t;

typedef struct ext4_node {
	struct vnode vnode;
	ext4_inode_t inode;
	struct ext4_node *next;
} ext4_node_t;

/*
 * ext4_mount_data
 *
 * Per-mount filesystem state.
 *
 * The block buffer is shared by metadata and file-data reads and is protected
 * by lock. Vnodes are resident for the lifetime of the mount and are indexed
 * by inode number through the node list.
 */
typedef struct {
	mount_t mount;
	block_device_t device;

	uint64_t blocks_count;
	uint32_t inodes_count;
	uint32_t first_inode;
	uint32_t first_data_block;
	uint32_t block_size;
	uint32_t blocks_per_group;
	uint32_t inodes_per_group;
	uint32_t group_count;

	uint16_t inode_size;
	uint16_t descriptor_size;

	uint32_t feature_compat;
	uint32_t feature_incompat;
	uint32_t feature_ro_compat;

	uint64_t free_blocks_count;
	uint32_t free_inodes_count;
	uint32_t next_generation;

	uint8_t uuid[16];
	uint32_t checksum_seed;
	uint32_t journal_inode;
	uint8_t journal_uuid[16];
	bool metadata_csum;
	bool recovery_required;

	jbd2_t journal;

	uint8_t superblock[EXT4_SUPERBLOCK_SIZE];
	uint8_t *block_buffer;
	uint8_t *inode_buffer;
	ext4_node_t *nodes;
	uint32_t node_count;

	volatile uint32_t lock;
	bool active;
} ext4_mount_data_t;

static vfs_status_t ext4_mount(filesystem_t filesystem, block_device_t device, mount_t mount);
static vfs_status_t ext4_sync(mount_t mount);
static vfs_status_t ext4_unmount(mount_t mount);
static vfs_status_t ext4_space_info(mount_t mount, vfs_space_info_t *info);
static vfs_status_t ext4_lookup(vnode_t directory, const char *name, vnode_t *result);
static vfs_status_t ext4_create(vnode_t directory, const char *name, vnode_type_t type, vnode_t *result);
static vfs_status_t ext4_unlink(vnode_t directory, const char *name);
static vfs_status_t ext4_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *entry);
static vfs_status_t ext4_read(vnode_t vnode, uint64_t offset, void *buffer, uint64_t size, uint64_t *read_size);
static vfs_status_t ext4_write(vnode_t vnode, uint64_t offset, const void *buffer, uint64_t size, uint64_t *written_size);
static vfs_status_t ext4_truncate(vnode_t vnode, uint64_t size);

static const filesystem_operations_t g_ext4_filesystem_operations = {
	.mount = ext4_mount,
	.sync = ext4_sync,
	.unmount = ext4_unmount,
	.space_info = ext4_space_info
};

static const vnode_operations_t g_ext4_vnode_operations = {
	.lookup = ext4_lookup,
	.create = ext4_create,
	.unlink = ext4_unlink,
	.readdir = ext4_readdir,
	.read = ext4_read,
	.write = ext4_write,
	.truncate = ext4_truncate
};

static struct vfs_filesystem g_ext4_filesystem = {
	.fs_name = "ext4",
	.fs_ops = &g_ext4_filesystem_operations
};

static ext4_mount_data_t *g_ext4_mounts[EXT4_MOUNT_MAX];
static uint32_t g_ext4_mount_count;

static uint16_t ext4_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
}

static uint32_t ext4_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0]
		| ((uint32_t)bytes[1] << 8U)
		| ((uint32_t)bytes[2] << 16U)
		| ((uint32_t)bytes[3] << 24U);
}

static void ext4_set_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8U);
}

static void ext4_set_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8U);
	bytes[2] = (uint8_t)(value >> 16U);
	bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t ext4_align4(uint32_t value)
{
	return (value + 3U) & ~3U;
}


/*
 * ext4_superblock_checksum:
 *
 * Compute the metadata_csum CRC32C covering the primary superblock up to,
 * but not including, s_checksum.
 */
static uint32_t ext4_superblock_checksum(const uint8_t super[EXT4_SUPERBLOCK_SIZE])
{
	return crc32c(~0U, super, 0x3FCU);
}

static void ext4_update_superblock_checksum(ext4_mount_data_t *data)
{
	if (!data->metadata_csum) return;
	ext4_set_le32(data->superblock + 0x3FCU, ext4_superblock_checksum(data->superblock));
}

static uint32_t ext4_inode_checksum(ext4_mount_data_t *data, uint32_t inode_number, uint8_t *raw)
{
	uint16_t saved_lo = ext4_le16(raw + 0x7CU);
	bool has_hi = data->inode_size > 0x83U && ext4_le16(raw + 0x80U) >= 4U;
	uint16_t saved_hi = has_hi ? ext4_le16(raw + 0x82U) : 0U;
	ext4_set_le16(raw + 0x7CU, 0U);
	if (has_hi) ext4_set_le16(raw + 0x82U, 0U);
	uint8_t number[4];
	uint8_t generation[4];
	ext4_set_le32(number, inode_number);
	ext4_set_le32(generation, ext4_le32(raw + 0x64U));
	uint32_t checksum = crc32c(data->checksum_seed, number, sizeof(number));
	checksum = crc32c(checksum, generation, sizeof(generation));
	checksum = crc32c(checksum, raw, data->inode_size);
	ext4_set_le16(raw + 0x7CU, saved_lo);
	if (has_hi) ext4_set_le16(raw + 0x82U, saved_hi);
	return checksum;
}

static bool ext4_verify_inode_checksum(ext4_mount_data_t *data, uint32_t inode_number, uint8_t *raw)
{
	if (!data->metadata_csum) return true;
	uint32_t provided = ext4_le16(raw + 0x7CU);
	bool has_hi = data->inode_size > 0x83U && ext4_le16(raw + 0x80U) >= 4U;
	if (has_hi) provided |= (uint32_t)ext4_le16(raw + 0x82U) << 16U;
	uint32_t calculated = ext4_inode_checksum(data, inode_number, raw);
	if (!has_hi) calculated &= 0xFFFFU;
	return provided == calculated;
}

static void ext4_set_inode_checksum(ext4_mount_data_t *data, uint32_t inode_number, uint8_t *raw)
{
	if (!data->metadata_csum) return;
	ext4_set_le16(raw + 0x7CU, 0U);
	bool has_hi = data->inode_size > 0x83U && ext4_le16(raw + 0x80U) >= 4U;
	if (has_hi) ext4_set_le16(raw + 0x82U, 0U);
	uint32_t checksum = ext4_inode_checksum(data, inode_number, raw);
	ext4_set_le16(raw + 0x7CU, (uint16_t)checksum);
	if (has_hi) ext4_set_le16(raw + 0x82U, (uint16_t)(checksum >> 16U));
}

static uint16_t ext4_group_checksum(ext4_mount_data_t *data, uint32_t group, uint8_t raw[64])
{
	uint16_t saved = ext4_le16(raw + 0x1EU);
	ext4_set_le16(raw + 0x1EU, 0U);
	uint8_t group_bytes[4];
	ext4_set_le32(group_bytes, group);
	uint32_t checksum = crc32c(data->checksum_seed, group_bytes, sizeof(group_bytes));
	checksum = crc32c(checksum, raw, data->descriptor_size);
	ext4_set_le16(raw + 0x1EU, saved);
	return (uint16_t)checksum;
}

static uint32_t ext4_block_bitmap_checksum(ext4_mount_data_t *data, const uint8_t *bitmap)
{
	uint32_t bytes = data->blocks_per_group / 8U;
	if (bytes > data->block_size) bytes = data->block_size;
	return crc32c(data->checksum_seed, bitmap, bytes);
}

static uint32_t ext4_inode_bitmap_checksum(ext4_mount_data_t *data, const uint8_t *bitmap)
{
	uint32_t bytes = data->inodes_per_group / 8U;
	if (bytes > data->block_size) bytes = data->block_size;
	return crc32c(data->checksum_seed, bitmap, bytes);
}

static bool ext4_verify_block_bitmap_checksum(ext4_mount_data_t *data, const uint8_t raw[64], const uint8_t *bitmap)
{
	uint32_t provided = ext4_le16(raw + 0x18U);
	uint32_t calculated = ext4_block_bitmap_checksum(data, bitmap);
	if (data->descriptor_size >= 64U) provided |= (uint32_t)ext4_le16(raw + 0x38U) << 16U;
	else calculated &= 0xFFFFU;
	return provided == calculated;
}

static void ext4_set_block_bitmap_checksum(ext4_mount_data_t *data, uint8_t raw[64], const uint8_t *bitmap)
{
	uint32_t checksum = ext4_block_bitmap_checksum(data, bitmap);
	ext4_set_le16(raw + 0x18U, (uint16_t)checksum);
	if (data->descriptor_size >= 64U) ext4_set_le16(raw + 0x38U, (uint16_t)(checksum >> 16U));
}

static bool ext4_verify_inode_bitmap_checksum(ext4_mount_data_t *data, const uint8_t raw[64], const uint8_t *bitmap)
{
	uint32_t provided = ext4_le16(raw + 0x1AU);
	uint32_t calculated = ext4_inode_bitmap_checksum(data, bitmap);
	if (data->descriptor_size >= 64U) provided |= (uint32_t)ext4_le16(raw + 0x3AU) << 16U;
	else calculated &= 0xFFFFU;
	return provided == calculated;
}

static void ext4_set_inode_bitmap_checksum(ext4_mount_data_t *data, uint8_t raw[64], const uint8_t *bitmap)
{
	uint32_t checksum = ext4_inode_bitmap_checksum(data, bitmap);
	ext4_set_le16(raw + 0x1AU, (uint16_t)checksum);
	if (data->descriptor_size >= 64U) ext4_set_le16(raw + 0x3AU, (uint16_t)(checksum >> 16U));
}

static uint32_t ext4_directory_checksum(ext4_mount_data_t *data, const ext4_inode_t *inode, const uint8_t *block)
{
	uint8_t number[4];
	uint8_t generation[4];
	ext4_set_le32(number, inode->number);
	ext4_set_le32(generation, inode->generation);
	uint32_t checksum = crc32c(data->checksum_seed, number, sizeof(number));
	checksum = crc32c(checksum, generation, sizeof(generation));
	return crc32c(checksum, block, data->block_size - EXT4_DIRECTORY_TAIL_SIZE);
}

static bool ext4_verify_directory_checksum(ext4_mount_data_t *data, const ext4_inode_t *inode, const uint8_t *block)
{
	if (!data->metadata_csum) return true;
	const uint8_t *tail = block + data->block_size - EXT4_DIRECTORY_TAIL_SIZE;
	if (ext4_le32(tail + 0x00U) != 0U) return false;

	if (ext4_le16(tail + 0x04U) != EXT4_DIRECTORY_TAIL_SIZE) return false;
	if (tail[0x06U] != 0U || tail[0x07U] != EXT4_DIRECTORY_TAIL_FT) return false;
	return ext4_le32(tail + 0x08U) == ext4_directory_checksum(data, inode, block);
}

static void ext4_set_directory_checksum(ext4_mount_data_t *data, const ext4_inode_t *inode, uint8_t *block)
{
	if (!data->metadata_csum) return;
	uint8_t *tail = block + data->block_size - EXT4_DIRECTORY_TAIL_SIZE;
	ext4_set_le32(tail + 0x00U, 0U);
	ext4_set_le16(tail + 0x04U, EXT4_DIRECTORY_TAIL_SIZE);
	tail[0x06U] = 0U;
	tail[0x07U] = EXT4_DIRECTORY_TAIL_FT;
	ext4_set_le32(tail + 0x08U, ext4_directory_checksum(data, inode, block));
}


static uint32_t ext4_extent_block_checksum(
	ext4_mount_data_t *data,
	const ext4_inode_t *inode,
	const uint8_t *block
)
{
	uint8_t number[4];
	uint8_t generation[4];
	ext4_set_le32(number, inode->number);
	ext4_set_le32(generation, inode->generation);
	uint32_t checksum = crc32c(data->checksum_seed, number, sizeof(number));
	checksum = crc32c(checksum, generation, sizeof(generation));
	return crc32c(checksum, block, data->block_size - 4U);
}

static bool ext4_verify_extent_block_checksum(
	ext4_mount_data_t *data,
	const ext4_inode_t *inode,
	const uint8_t *block
)
{
	if (!data->metadata_csum) return true;
	return ext4_le32(block + data->block_size - 4U) == ext4_extent_block_checksum(data, inode, block);
}

static void ext4_lock(ext4_mount_data_t *data)
{
	while (__atomic_exchange_n(&data->lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void ext4_unlock(ext4_mount_data_t *data)
{
	__atomic_store_n(&data->lock, 0U, __ATOMIC_RELEASE);
}

static bool ext4_read_block_locked(ext4_mount_data_t *data, uint64_t block, void *buffer);
static bool ext4_write_metadata_block_locked(ext4_mount_data_t *data, uint64_t block, const void *buffer);

/*
 * ext4_read_bytes:
 *
 * Read an arbitrary byte range through the sector-addressed block layer.
 * Whole sectors bypass the temporary sector buffer; unaligned edges are
 * copied from one sector at a time.
 */
static bool ext4_read_bytes(
	ext4_mount_data_t *data,
	uint64_t offset,
	void *buffer,
	uint64_t size
)
{
	if (data == 0 || buffer == 0) return false;

	if (size == 0ULL) return true;

	uint64_t device_size = data->device->sector_count * BLOCK_SECTOR_SIZE;
	if (offset > device_size || size > device_size - offset) return false;

	uint8_t *destination = buffer;
	if (data->block_size != 0U && jbd2_is_transaction_active(&data->journal)) {
		uint8_t scratch[EXT4_MAX_BLOCK_SIZE];
		while (size != 0ULL) {
			uint64_t block = offset / data->block_size;
			uint32_t block_offset = (uint32_t)(offset % data->block_size);
			uint32_t available = data->block_size - block_offset;
			uint32_t amount = size < available ? (uint32_t)size : available;
			if (!ext4_read_block_locked(data, block, scratch)) return false;
			memcpy(destination, scratch + block_offset, amount);
			offset += amount;
			destination += amount;
			size -= amount;
		}
		return true;
	}

	uint8_t sector_buffer[BLOCK_SECTOR_SIZE];

	while (size != 0ULL) {
		uint64_t sector = offset / BLOCK_SECTOR_SIZE;
		uint32_t sector_offset = (uint32_t)(offset % BLOCK_SECTOR_SIZE);

		if (sector_offset == 0U && size >= BLOCK_SECTOR_SIZE) {
			uint64_t whole = size / BLOCK_SECTOR_SIZE;
			uint32_t count = whole > UINT32_MAX ? UINT32_MAX : (uint32_t)whole;
			if (!block_device_read(data->device, sector, count, destination)) return false;

			uint64_t bytes = (uint64_t)count * BLOCK_SECTOR_SIZE;
			offset += bytes;
			destination += bytes;
			size -= bytes;
			continue;
		}

		if (!block_device_read(data->device, sector, 1U, sector_buffer)) return false;

		uint32_t available = BLOCK_SECTOR_SIZE - sector_offset;
		uint32_t amount = size < available ? (uint32_t)size : available;
		memcpy(destination, sector_buffer + sector_offset, amount);

		offset += amount;
		destination += amount;
		size -= amount;
	}

	return true;
}


/*
 * ext4_write_bytes:
 *
 * Write an arbitrary byte range through the sector-addressed block layer.
 * Partial sectors are preserved with read/modify/write cycles.
 */
static bool ext4_write_bytes(
	ext4_mount_data_t *data,
	uint64_t offset,
	const void *buffer,
	uint64_t size
)
{
	if (data == 0 || buffer == 0) return false;

	if (size == 0ULL) return true;
	if (data->device->read_only) return false;
	uint64_t device_size = data->device->sector_count * BLOCK_SECTOR_SIZE;
	if (offset > device_size || size > device_size - offset) return false;

	if (data->block_size != 0U && jbd2_is_transaction_active(&data->journal)) {
		const uint8_t *source = buffer;
		uint8_t scratch[EXT4_MAX_BLOCK_SIZE];
		while (size != 0ULL) {
			uint64_t block = offset / data->block_size;
			uint32_t block_offset = (uint32_t)(offset % data->block_size);
			uint32_t available = data->block_size - block_offset;
			uint32_t amount = size < available ? (uint32_t)size : available;
			if (block_offset == 0U && amount == data->block_size) {
				if (!ext4_write_metadata_block_locked(data, block, source)) return false;
			} else {
				if (!ext4_read_block_locked(data, block, scratch)) return false;
				memcpy(scratch + block_offset, source, amount);
				if (!ext4_write_metadata_block_locked(data, block, scratch)) return false;
			}
			offset += amount;
			source += amount;
			size -= amount;
		}
		return true;
	}

	const uint8_t *source = buffer;
	uint8_t sector_buffer[BLOCK_SECTOR_SIZE];
	while (size != 0ULL) {
		uint64_t sector = offset / BLOCK_SECTOR_SIZE;
		uint32_t sector_offset = (uint32_t)(offset % BLOCK_SECTOR_SIZE);
		if (sector_offset == 0U && size >= BLOCK_SECTOR_SIZE) {
			uint64_t whole = size / BLOCK_SECTOR_SIZE;
			uint32_t count = whole > UINT32_MAX ? UINT32_MAX : (uint32_t)whole;
			if (!block_device_write(data->device, sector, count, source)) return false;
			uint64_t bytes = (uint64_t)count * BLOCK_SECTOR_SIZE;
			offset += bytes;
			source += bytes;
			size -= bytes;
			continue;
		}

		if (!block_device_read(data->device, sector, 1U, sector_buffer)) return false;
		uint32_t available = BLOCK_SECTOR_SIZE - sector_offset;
		uint32_t amount = size < available ? (uint32_t)size : available;
		memcpy(sector_buffer + sector_offset, source, amount);
		if (!block_device_write(data->device, sector, 1U, sector_buffer)) return false;
		offset += amount;
		source += amount;
		size -= amount;
	}
	return true;
}

/*
 * ext4_read_block_locked:
 *
 * Read one filesystem block. The caller holds the mount lock and owns the
 * destination buffer for at least block_size bytes.
 */
static bool ext4_read_block_locked(ext4_mount_data_t *data, uint64_t block, void *buffer)
{
	if (block >= data->blocks_count) return false;

	if (jbd2_overlay(&data->journal, block, buffer)) return true;
	uint64_t sectors_per_block = data->block_size / BLOCK_SECTOR_SIZE;
	uint64_t sector = block * sectors_per_block;
	if (sector > UINT64_MAX - sectors_per_block) return false;
	return block_device_read(data->device, sector, (uint32_t)sectors_per_block, buffer);
}


/*
 * ext4_write_block_locked:
 *
 * Write one complete filesystem block. The caller holds the mount lock.
 */
static bool ext4_write_block_locked(ext4_mount_data_t *data, uint64_t block, const void *buffer)
{
	if (block >= data->blocks_count) return false;
	uint64_t sectors_per_block = data->block_size / BLOCK_SECTOR_SIZE;
	uint64_t sector = block * sectors_per_block;
	if (sector > UINT64_MAX - sectors_per_block) return false;
	return block_device_write(data->device, sector, (uint32_t)sectors_per_block, buffer);
}


static bool ext4_write_metadata_block_locked(ext4_mount_data_t *data, uint64_t block, const void *buffer)
{
	if (jbd2_is_transaction_active(&data->journal)) {
		return jbd2_stage(&data->journal, block, buffer) == JBD2_STATUS_OK;
	}
	return ext4_write_block_locked(data, block, buffer);
}

static vfs_status_t ext4_jbd2_status(jbd2_status_t status)
{
	switch (status) {
	case JBD2_STATUS_OK: return VFS_STATUS_OK;
	case JBD2_STATUS_NO_MEMORY: return VFS_STATUS_NO_MEMORY;
	case JBD2_STATUS_NO_SPACE: return VFS_STATUS_NO_SPACE;
	case JBD2_STATUS_NOT_SUPPORTED: return VFS_STATUS_NOT_SUPPORTED;
	default: return VFS_STATUS_IO_ERROR;
	}
}

static vfs_status_t ext4_set_recovery_flag_locked(ext4_mount_data_t *data, bool required)
{
	if (required) data->feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
	else data->feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
	ext4_set_le32(data->superblock + 0x60U, data->feature_incompat);
	ext4_update_superblock_checksum(data);
	if (!ext4_write_bytes(data, EXT4_SUPERBLOCK_OFFSET, data->superblock, EXT4_SUPERBLOCK_SIZE)) {
		return VFS_STATUS_IO_ERROR;
	}

	if (!block_device_flush(data->device)) return VFS_STATUS_IO_ERROR;
	data->recovery_required = required;
	return VFS_STATUS_OK;
}

/*
 * ext4_transaction_begin_locked:
 *
 * Make needs-recovery durable before metadata enters the journal. The mount
 * lock serializes the one active transaction supported by this foundation.
 */
static vfs_status_t ext4_transaction_begin_locked(ext4_mount_data_t *data)
{
	if (!data->journal.active) return VFS_STATUS_IO_ERROR;
	vfs_status_t status = ext4_set_recovery_flag_locked(data, true);
	if (status != VFS_STATUS_OK) return status;
	status = ext4_jbd2_status(jbd2_begin(&data->journal));
	if (status != VFS_STATUS_OK) (void)ext4_set_recovery_flag_locked(data, false);
	return status;
}

static void ext4_transaction_abort_locked(ext4_mount_data_t *data)
{
	jbd2_abort(&data->journal);
	(void)ext4_set_recovery_flag_locked(data, false);
}

static vfs_status_t ext4_transaction_commit_locked(ext4_mount_data_t *data)
{
	jbd2_status_t journal_status = jbd2_commit(&data->journal);
	if (journal_status == JBD2_STATUS_CRASH_POINT) return VFS_STATUS_IO_ERROR;
	vfs_status_t status = ext4_jbd2_status(journal_status);
	if (status != VFS_STATUS_OK) return status;
	return ext4_set_recovery_flag_locked(data, false);
}

/*
 * ext4_parse_superblock:
 *
 * Validate the primary superblock and retain fields required by the ext4
 * implementation. Unknown incompatible features reject the mount. Features
 * which require metadata maintenance outside the writable foundation are
 * rejected before the filesystem is published.
 */
static vfs_status_t ext4_parse_superblock(ext4_mount_data_t *data, const uint8_t super[EXT4_SUPERBLOCK_SIZE])
{
	if (ext4_le16(super + 0x38U) != EXT4_SUPER_MAGIC) return VFS_STATUS_NOT_FOUND;
	uint32_t compat = ext4_le32(super + 0x5CU);
	uint32_t incompat = ext4_le32(super + 0x60U);
	uint32_t ro_compat = ext4_le32(super + 0x64U);
	if ((ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) == 0U) return VFS_STATUS_NOT_SUPPORTED;

	if (super[0x175U] != EXT4_CHECKSUM_TYPE_CRC32C) return VFS_STATUS_NOT_SUPPORTED;
	if (ext4_superblock_checksum(super) != ext4_le32(super + 0x3FCU)) return VFS_STATUS_IO_ERROR;

	uint32_t log_block_size = ext4_le32(super + 0x18U);
	if (log_block_size > 2U) return VFS_STATUS_NOT_SUPPORTED;
	uint32_t block_size = 1024U << log_block_size;
	if (block_size < EXT4_MIN_BLOCK_SIZE || block_size > EXT4_MAX_BLOCK_SIZE) return VFS_STATUS_NOT_SUPPORTED;

	if ((block_size % BLOCK_SECTOR_SIZE) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	if (ext4_le32(super + 0x1CU) != log_block_size) return VFS_STATUS_NOT_SUPPORTED;
	uint16_t filesystem_state = ext4_le16(super + 0x3AU);
	if ((filesystem_state & 0x0001U) == 0U && (incompat & EXT4_FEATURE_INCOMPAT_RECOVER) == 0U) return VFS_STATUS_NOT_SUPPORTED;

	if ((compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) == 0U) return VFS_STATUS_NOT_SUPPORTED;

	if ((compat & EXT4_FEATURE_COMPAT_DIR_INDEX) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	if ((incompat & ~EXT4_SUPPORTED_INCOMPAT) != 0U) return VFS_STATUS_NOT_SUPPORTED;

	if ((incompat & EXT4_FEATURE_INCOMPAT_META_BG) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	if ((incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) != 0U) return VFS_STATUS_NOT_SUPPORTED;

	if ((incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	if ((incompat & EXT4_FEATURE_INCOMPAT_CASEFOLD) != 0U) return VFS_STATUS_NOT_SUPPORTED;

	if ((incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) == 0U) return VFS_STATUS_NOT_SUPPORTED;
	if ((ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) != 0U) return VFS_STATUS_NOT_SUPPORTED;

	if ((ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	if ((ro_compat & EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT) != 0U) return VFS_STATUS_NOT_SUPPORTED;

	uint32_t blocks_lo = ext4_le32(super + 0x04U);
	uint32_t blocks_hi = (incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0U ? ext4_le32(super + 0x150U) : 0U;
	uint64_t blocks_count = (uint64_t)blocks_lo | ((uint64_t)blocks_hi << 32U);
	uint32_t first_data_block = ext4_le32(super + 0x14U);
	uint32_t blocks_per_group = ext4_le32(super + 0x20U);
	uint32_t inodes_per_group = ext4_le32(super + 0x28U);
	uint32_t inodes_count = ext4_le32(super + 0x00U);
	uint32_t first_inode = ext4_le32(super + 0x54U);
	uint16_t inode_size = ext4_le16(super + 0x58U);
	uint32_t journal_inode = ext4_le32(super + 0xE0U);
	if (blocks_count == 0ULL || blocks_per_group == 0U || inodes_per_group == 0U) return VFS_STATUS_IO_ERROR;

	if (inodes_count < EXT4_ROOT_INODE || journal_inode != EXT4_JOURNAL_INODE_DEFAULT) return VFS_STATUS_NOT_SUPPORTED;
	if (first_inode < 11U || first_inode > inodes_count) return VFS_STATUS_IO_ERROR;

	if (first_data_block >= blocks_count) return VFS_STATUS_IO_ERROR;
	if (inode_size < EXT4_GOOD_OLD_INODE_SIZE || inode_size > block_size || (inode_size & 3U) != 0U) return VFS_STATUS_NOT_SUPPORTED;

	if (blocks_count > UINT64_MAX / block_size) return VFS_STATUS_IO_ERROR;
	uint64_t addressable_bytes = blocks_count * block_size;
	if (data->device->sector_count > UINT64_MAX / BLOCK_SECTOR_SIZE) return VFS_STATUS_IO_ERROR;

	if (addressable_bytes > data->device->sector_count * BLOCK_SECTOR_SIZE) return VFS_STATUS_IO_ERROR;
	uint64_t described_blocks = blocks_count - first_data_block;
	uint64_t groups = (described_blocks + blocks_per_group - 1ULL) / blocks_per_group;
	if (groups == 0ULL || groups > UINT32_MAX) return VFS_STATUS_NOT_SUPPORTED;
	uint16_t descriptor_size = 32U;
	if ((incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0U) {
		descriptor_size = ext4_le16(super + 0xFEU);
		if (descriptor_size != 64U) return VFS_STATUS_NOT_SUPPORTED;
	}

	data->inodes_count = inodes_count;
	data->first_inode = first_inode;
	data->blocks_count = blocks_count;
	data->first_data_block = first_data_block;
	data->block_size = block_size;
	data->blocks_per_group = blocks_per_group;
	data->inodes_per_group = inodes_per_group;
	data->group_count = (uint32_t)groups;
	data->inode_size = inode_size;
	data->descriptor_size = descriptor_size;
	data->feature_compat = compat;
	data->feature_incompat = incompat;
	data->feature_ro_compat = ro_compat;
	data->metadata_csum = true;
	data->recovery_required = (incompat & EXT4_FEATURE_INCOMPAT_RECOVER) != 0U;
	data->journal_inode = journal_inode;
	memcpy(data->uuid, super + 0x68U, 16U);
	memcpy(data->journal_uuid, super + 0xD0U, 16U);
	data->checksum_seed = (incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) != 0U ? ext4_le32(super + 0x270U) : crc32c(~0U, data->uuid, 16U);
	data->free_inodes_count = ext4_le32(super + 0x10U);
	data->free_blocks_count = ext4_le32(super + 0x0CU);
	if ((incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0U) data->free_blocks_count |= (uint64_t)ext4_le32(super + 0x158U) << 32U;
	memcpy(data->superblock, super, EXT4_SUPERBLOCK_SIZE);
	data->next_generation = 1U;
	return VFS_STATUS_OK;
}

/*
 * ext4_inode_table_block_locked:
 *
 * Locate a block group's inode table through the primary descriptor table.
 * meta_bg is rejected at mount time, so every descriptor is reachable from
 * the descriptor table immediately following the primary superblock.
 */
static vfs_status_t ext4_inode_table_block_locked(
	ext4_mount_data_t *data,
	uint32_t group,
	uint64_t *table_block
)
{
	if (group >= data->group_count || table_block == 0) return VFS_STATUS_INVALID;

	uint64_t descriptor_table_block = (uint64_t)data->first_data_block + 1ULL;
	uint64_t descriptor_offset = descriptor_table_block * data->block_size
		+ (uint64_t)group * data->descriptor_size;

	uint8_t descriptor[64];
	uint32_t bytes = data->descriptor_size < sizeof(descriptor)
		? data->descriptor_size
		: (uint32_t)sizeof(descriptor);

	memset(descriptor, 0, sizeof(descriptor));
	if (!ext4_read_bytes(data, descriptor_offset, descriptor, bytes)) return VFS_STATUS_IO_ERROR;

	uint64_t block = ext4_le32(descriptor + 0x08U);
	if (data->descriptor_size >= 64U) block |= (uint64_t)ext4_le32(descriptor + 0x28U) << 32U;
	if (block == 0ULL || block >= data->blocks_count) return VFS_STATUS_IO_ERROR;

	*table_block = block;
	return VFS_STATUS_OK;
}


typedef struct {
	uint64_t block_bitmap;
	uint64_t inode_bitmap;
	uint64_t inode_table;
	uint32_t free_blocks_count;
	uint32_t free_inodes_count;
	uint32_t used_dirs_count;
	uint32_t itable_unused_count;
	uint16_t flags;
} ext4_group_info_t;

/*
 * ext4_group_descriptor_offset:
 *
 * Return the byte offset of one primary block-group descriptor. meta_bg is
 * rejected at mount time, so the primary descriptor table is contiguous.
 */
static uint64_t ext4_group_descriptor_offset(ext4_mount_data_t *data, uint32_t group)
{
	uint64_t descriptor_table_block = (uint64_t)data->first_data_block + 1ULL;
	return descriptor_table_block * data->block_size + (uint64_t)group * data->descriptor_size;
}

static vfs_status_t ext4_read_group_locked(
	ext4_mount_data_t *data,
	uint32_t group,
	ext4_group_info_t *info,
	uint8_t raw[64]
)
{
	if (group >= data->group_count || info == 0 || raw == 0) return VFS_STATUS_INVALID;
	memset(raw, 0, 64U);
	uint32_t bytes = data->descriptor_size < 64U ? data->descriptor_size : 64U;
	if (!ext4_read_bytes(data, ext4_group_descriptor_offset(data, group), raw, bytes)) {
		return VFS_STATUS_IO_ERROR;
	}

	if (data->metadata_csum && ext4_le16(raw + 0x1EU) != ext4_group_checksum(data, group, raw)) return VFS_STATUS_IO_ERROR;

	info->block_bitmap = ext4_le32(raw + 0x00U);
	info->inode_bitmap = ext4_le32(raw + 0x04U);
	info->inode_table = ext4_le32(raw + 0x08U);
	info->free_blocks_count = ext4_le16(raw + 0x0CU);
	info->free_inodes_count = ext4_le16(raw + 0x0EU);
	info->used_dirs_count = ext4_le16(raw + 0x10U);
	info->itable_unused_count = ext4_le16(raw + 0x1CU);
	info->flags = ext4_le16(raw + 0x12U);

	if (data->descriptor_size >= 64U) {
		info->block_bitmap |= (uint64_t)ext4_le32(raw + 0x20U) << 32U;
		info->inode_bitmap |= (uint64_t)ext4_le32(raw + 0x24U) << 32U;
		info->inode_table |= (uint64_t)ext4_le32(raw + 0x28U) << 32U;
		info->free_blocks_count |= (uint32_t)ext4_le16(raw + 0x2CU) << 16U;
		info->free_inodes_count |= (uint32_t)ext4_le16(raw + 0x2EU) << 16U;
		info->used_dirs_count |= (uint32_t)ext4_le16(raw + 0x30U) << 16U;
		info->itable_unused_count |= (uint32_t)ext4_le16(raw + 0x32U) << 16U;
	}

	if (
		info->block_bitmap == 0ULL || info->block_bitmap >= data->blocks_count ||
		info->inode_bitmap == 0ULL || info->inode_bitmap >= data->blocks_count ||
		info->inode_table == 0ULL || info->inode_table >= data->blocks_count
	) {
		return VFS_STATUS_IO_ERROR;
	}

	return VFS_STATUS_OK;
}

static vfs_status_t ext4_write_group_locked(
	ext4_mount_data_t *data,
	uint32_t group,
	const ext4_group_info_t *info,
	uint8_t raw[64]
)
{
	if (group >= data->group_count || info == 0 || raw == 0) return VFS_STATUS_INVALID;

	ext4_set_le16(raw + 0x0CU, (uint16_t)info->free_blocks_count);
	ext4_set_le16(raw + 0x0EU, (uint16_t)info->free_inodes_count);
	ext4_set_le16(raw + 0x10U, (uint16_t)info->used_dirs_count);
	ext4_set_le16(raw + 0x1CU, (uint16_t)info->itable_unused_count);
	if (data->descriptor_size >= 64U) {
		ext4_set_le16(raw + 0x2CU, (uint16_t)(info->free_blocks_count >> 16U));
		ext4_set_le16(raw + 0x2EU, (uint16_t)(info->free_inodes_count >> 16U));
		ext4_set_le16(raw + 0x30U, (uint16_t)(info->used_dirs_count >> 16U));
		ext4_set_le16(raw + 0x32U, (uint16_t)(info->itable_unused_count >> 16U));
	}

	if (data->metadata_csum) {
		ext4_set_le16(raw + 0x1EU, 0U);
		ext4_set_le16(raw + 0x1EU, ext4_group_checksum(data, group, raw));
	}
	uint32_t bytes = data->descriptor_size < 64U ? data->descriptor_size : 64U;
	return ext4_write_bytes(data, ext4_group_descriptor_offset(data, group), raw, bytes)
		? VFS_STATUS_OK
		: VFS_STATUS_IO_ERROR;
}

/*
 * ext4_write_superblock_locked:
 *
 * Persist free-space counters maintained by the writable foundation. The
 * primary superblock checksum is refreshed after all mutable fields are set.
 */
static vfs_status_t ext4_write_superblock_locked(ext4_mount_data_t *data)
{
	ext4_set_le32(data->superblock + 0x0CU, (uint32_t)data->free_blocks_count);
	ext4_set_le32(data->superblock + 0x10U, data->free_inodes_count);
	if ((data->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0U) {
		ext4_set_le32(data->superblock + 0x158U, (uint32_t)(data->free_blocks_count >> 32U));
	}
	ext4_set_le32(data->superblock + 0x60U, data->feature_incompat);
	ext4_update_superblock_checksum(data);

	return ext4_write_bytes(data, EXT4_SUPERBLOCK_OFFSET, data->superblock, EXT4_SUPERBLOCK_SIZE)
		? VFS_STATUS_OK
		: VFS_STATUS_IO_ERROR;
}

static bool ext4_bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
	return (bitmap[bit >> 3U] & (uint8_t)(1U << (bit & 7U))) != 0U;
}

static void ext4_bitmap_set(uint8_t *bitmap, uint32_t bit)
{
	bitmap[bit >> 3U] |= (uint8_t)(1U << (bit & 7U));
}

static void ext4_bitmap_clear(uint8_t *bitmap, uint32_t bit)
{
	bitmap[bit >> 3U] &= (uint8_t)~(1U << (bit & 7U));
}

/*
 * ext4_allocate_block_locked:
 *
 * Allocate one data block, preferring the inode's block group. Bitmap and
 * free-space counters are persisted before the block is returned.
 */
static vfs_status_t ext4_allocate_block_locked(
	ext4_mount_data_t *data,
	uint32_t preferred_group,
	uint64_t *block
)
{
	if (block == 0) return VFS_STATUS_INVALID;
	*block = 0ULL;
	if (data->free_blocks_count == 0ULL) return VFS_STATUS_NO_SPACE;

	for (uint32_t pass = 0U; pass < data->group_count; pass++) {
		uint32_t group = (preferred_group + pass) % data->group_count;
		ext4_group_info_t info;
		uint8_t raw[64];
		vfs_status_t status = ext4_read_group_locked(data, group, &info, raw);
		if (status != VFS_STATUS_OK) return status;

		if ((info.flags & EXT4_BG_BLOCK_UNINIT) != 0U) return VFS_STATUS_NOT_SUPPORTED;
		if (info.free_blocks_count == 0U) continue;

		if (!ext4_read_block_locked(data, info.block_bitmap, data->block_buffer)) {
			return VFS_STATUS_IO_ERROR;
		}

		if (!ext4_verify_block_bitmap_checksum(data, raw, data->block_buffer)) return VFS_STATUS_IO_ERROR;

		uint64_t group_start = (uint64_t)data->first_data_block + (uint64_t)group * data->blocks_per_group;
		uint64_t remaining = data->blocks_count - group_start;
		uint32_t bit_count = remaining < data->blocks_per_group ? (uint32_t)remaining : data->blocks_per_group;

		for (uint32_t bit = 0U; bit < bit_count; bit++) {
			if (ext4_bitmap_test(data->block_buffer, bit)) continue;

			ext4_bitmap_set(data->block_buffer, bit);
			ext4_set_block_bitmap_checksum(data, raw, data->block_buffer);
			if (!ext4_write_metadata_block_locked(data, info.block_bitmap, data->block_buffer)) {
				return VFS_STATUS_IO_ERROR;
			}

			info.free_blocks_count--;
			data->free_blocks_count--;
			status = ext4_write_group_locked(data, group, &info, raw);
			if (status != VFS_STATUS_OK) return status;
			status = ext4_write_superblock_locked(data);
			if (status != VFS_STATUS_OK) return status;

			*block = group_start + bit;
			return VFS_STATUS_OK;
		}
	}

	return VFS_STATUS_NO_SPACE;
}

static vfs_status_t ext4_free_block_locked(ext4_mount_data_t *data, uint64_t block)
{
	if (block < data->first_data_block || block >= data->blocks_count) return VFS_STATUS_INVALID;

	uint64_t relative = block - data->first_data_block;
	uint32_t group = (uint32_t)(relative / data->blocks_per_group);
	uint32_t bit = (uint32_t)(relative % data->blocks_per_group);
	if (group >= data->group_count) return VFS_STATUS_INVALID;

	ext4_group_info_t info;
	uint8_t raw[64];
	vfs_status_t status = ext4_read_group_locked(data, group, &info, raw);
	if (status != VFS_STATUS_OK) return status;

	if ((info.flags & EXT4_BG_BLOCK_UNINIT) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	if (!ext4_read_block_locked(data, info.block_bitmap, data->block_buffer)) return VFS_STATUS_IO_ERROR;

	if (!ext4_verify_block_bitmap_checksum(data, raw, data->block_buffer)) return VFS_STATUS_IO_ERROR;
	if (!ext4_bitmap_test(data->block_buffer, bit)) return VFS_STATUS_IO_ERROR;

	ext4_bitmap_clear(data->block_buffer, bit);
	ext4_set_block_bitmap_checksum(data, raw, data->block_buffer);
	if (!ext4_write_metadata_block_locked(data, info.block_bitmap, data->block_buffer)) return VFS_STATUS_IO_ERROR;
	info.free_blocks_count++;
	data->free_blocks_count++;
	status = ext4_write_group_locked(data, group, &info, raw);
	if (status != VFS_STATUS_OK) return status;
	return ext4_write_superblock_locked(data);
}

/*
 * ext4_inode_offset_locked:
 *
 * Return the byte offset of one inode-table record.
 */
static vfs_status_t ext4_inode_offset_locked(
	ext4_mount_data_t *data,
	uint32_t inode_number,
	uint64_t *offset
)
{
	if (offset == 0 || inode_number == 0U || inode_number > data->inodes_count) return VFS_STATUS_INVALID;
	uint32_t group = (inode_number - 1U) / data->inodes_per_group;
	uint32_t index = (inode_number - 1U) % data->inodes_per_group;
	uint64_t table_block;
	vfs_status_t status = ext4_inode_table_block_locked(data, group, &table_block);
	if (status != VFS_STATUS_OK) return status;
	*offset = table_block * data->block_size + (uint64_t)index * data->inode_size;
	return VFS_STATUS_OK;
}

static vfs_status_t ext4_read_inode_raw_locked(
	ext4_mount_data_t *data,
	uint32_t inode_number,
	uint8_t *raw
)
{
	uint64_t offset;
	vfs_status_t status = ext4_inode_offset_locked(data, inode_number, &offset);
	if (status != VFS_STATUS_OK) return status;

	if (!ext4_read_bytes(data, offset, raw, data->inode_size)) return VFS_STATUS_IO_ERROR;
	return ext4_verify_inode_checksum(data, inode_number, raw) ? VFS_STATUS_OK : VFS_STATUS_IO_ERROR;
}

static vfs_status_t ext4_write_inode_raw_locked(
	ext4_mount_data_t *data,
	uint32_t inode_number,
	uint8_t *raw
)
{
	uint64_t offset;
	vfs_status_t status = ext4_inode_offset_locked(data, inode_number, &offset);
	if (status != VFS_STATUS_OK) return status;
	ext4_set_inode_checksum(data, inode_number, raw);
	return ext4_write_bytes(data, offset, raw, data->inode_size) ? VFS_STATUS_OK : VFS_STATUS_IO_ERROR;
}

static vfs_status_t ext4_allocate_inode_locked(
	ext4_mount_data_t *data,
	bool directory,
	uint32_t preferred_group,
	uint32_t *inode_number
)
{
	if (inode_number == 0) return VFS_STATUS_INVALID;
	*inode_number = 0U;
	if (data->free_inodes_count == 0U) return VFS_STATUS_NO_SPACE;

	for (uint32_t pass = 0U; pass < data->group_count; pass++) {
		uint32_t group = (preferred_group + pass) % data->group_count;
		ext4_group_info_t info;
		uint8_t raw[64];
		vfs_status_t status = ext4_read_group_locked(data, group, &info, raw);
		if (status != VFS_STATUS_OK) return status;

		if ((info.flags & EXT4_BG_INODE_UNINIT) != 0U) return VFS_STATUS_NOT_SUPPORTED;
		if (info.free_inodes_count == 0U) continue;

		if (!ext4_read_block_locked(data, info.inode_bitmap, data->block_buffer)) return VFS_STATUS_IO_ERROR;
		if (!ext4_verify_inode_bitmap_checksum(data, raw, data->block_buffer)) return VFS_STATUS_IO_ERROR;

		uint64_t group_first = (uint64_t)group * data->inodes_per_group + 1ULL;
		uint64_t remaining = data->inodes_count - group_first + 1ULL;
		uint32_t bit_count = remaining < data->inodes_per_group ? (uint32_t)remaining : data->inodes_per_group;

		for (uint32_t bit = 0U; bit < bit_count; bit++) {
			uint32_t candidate = (uint32_t)group_first + bit;
			if (candidate < data->first_inode) continue;

			if (ext4_bitmap_test(data->block_buffer, bit)) continue;

			ext4_bitmap_set(data->block_buffer, bit);
			uint32_t trailing_unused = bit_count - bit - 1U;
			if (info.itable_unused_count > trailing_unused) info.itable_unused_count = trailing_unused;
			ext4_set_inode_bitmap_checksum(data, raw, data->block_buffer);
			if (!ext4_write_metadata_block_locked(data, info.inode_bitmap, data->block_buffer)) return VFS_STATUS_IO_ERROR;
			info.free_inodes_count--;
			if (directory) info.used_dirs_count++;
			data->free_inodes_count--;
			status = ext4_write_group_locked(data, group, &info, raw);
			if (status != VFS_STATUS_OK) return status;
			status = ext4_write_superblock_locked(data);
			if (status != VFS_STATUS_OK) return status;

			*inode_number = candidate;
			return VFS_STATUS_OK;
		}
	}

	return VFS_STATUS_NO_SPACE;
}

static vfs_status_t ext4_free_inode_locked(ext4_mount_data_t *data, uint32_t inode_number, bool directory)
{
	if (inode_number < data->first_inode || inode_number > data->inodes_count) return VFS_STATUS_INVALID;
	uint32_t group = (inode_number - 1U) / data->inodes_per_group;
	uint32_t bit = (inode_number - 1U) % data->inodes_per_group;

	ext4_group_info_t info;
	uint8_t raw[64];
	vfs_status_t status = ext4_read_group_locked(data, group, &info, raw);
	if (status != VFS_STATUS_OK) return status;

	if ((info.flags & EXT4_BG_INODE_UNINIT) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	if (!ext4_read_block_locked(data, info.inode_bitmap, data->block_buffer)) return VFS_STATUS_IO_ERROR;

	if (!ext4_verify_inode_bitmap_checksum(data, raw, data->block_buffer)) return VFS_STATUS_IO_ERROR;
	if (!ext4_bitmap_test(data->block_buffer, bit)) return VFS_STATUS_IO_ERROR;

	ext4_bitmap_clear(data->block_buffer, bit);
	ext4_set_inode_bitmap_checksum(data, raw, data->block_buffer);
	if (!ext4_write_metadata_block_locked(data, info.inode_bitmap, data->block_buffer)) return VFS_STATUS_IO_ERROR;
	info.free_inodes_count++;
	if (directory && info.used_dirs_count != 0U) info.used_dirs_count--;
	data->free_inodes_count++;
	status = ext4_write_group_locked(data, group, &info, raw);
	if (status != VFS_STATUS_OK) return status;
	status = ext4_write_superblock_locked(data);
	if (status != VFS_STATUS_OK) return status;

	memset(data->inode_buffer, 0, data->inode_size);
	return ext4_write_inode_raw_locked(data, inode_number, data->inode_buffer);
}

/*
 * ext4_load_inode_locked:
 *
 * Load one inode record from its group-local inode table and decode the
 * subset needed for type, size and extent traversal.
 */
static vfs_status_t ext4_load_inode_locked(
	ext4_mount_data_t *data,
	uint32_t inode_number,
	ext4_inode_t *inode
)
{
	if (inode == 0 || inode_number == 0U || inode_number > data->inodes_count) return VFS_STATUS_INVALID;
	vfs_status_t status = ext4_read_inode_raw_locked(data, inode_number, data->inode_buffer);
	if (status != VFS_STATUS_OK) return status;
	const uint8_t *raw = data->inode_buffer;
	uint16_t mode = ext4_le16(raw + 0x00U);
	uint16_t links_count = ext4_le16(raw + 0x1AU);
	uint32_t flags = ext4_le32(raw + 0x20U);
	uint64_t blocks_512 = ext4_le32(raw + 0x1CU) | ((uint64_t)ext4_le16(raw + 0x74U) << 32U);
	uint64_t size = ext4_le32(raw + 0x04U);
	if ((mode & EXT4_MODE_TYPE_MASK) == EXT4_MODE_REGULAR || (mode & EXT4_MODE_TYPE_MASK) == EXT4_MODE_DIRECTORY) {
		size |= (uint64_t)ext4_le32(raw + 0x6CU) << 32U;
	}

	if ((flags & EXT4_INLINE_DATA_FL) != 0U || (flags & EXT4_EXTENTS_FL) == 0U) return VFS_STATUS_NOT_SUPPORTED;
	memset(inode, 0, sizeof(*inode));
	inode->number = inode_number;
	inode->mode = mode;
	inode->flags = flags;
	inode->size = size;
	inode->generation = ext4_le32(raw + 0x64U);
	inode->links_count = links_count;
	inode->blocks_512 = blocks_512;
	memcpy(inode->block, raw + 0x28U, sizeof(inode->block));
	return VFS_STATUS_OK;
}


/*
 * ext4_store_inode_locked:
 *
 * Encode mutable inode state while preserving extended inode fields that the
 * writable foundation does not own. The inode checksum is regenerated over
 * the final image before the containing inode-table block is journaled.
 */
static vfs_status_t ext4_store_inode_locked(ext4_mount_data_t *data, const ext4_inode_t *inode)
{
	if (inode == 0) return VFS_STATUS_INVALID;
	vfs_status_t status = ext4_read_inode_raw_locked(data, inode->number, data->inode_buffer);
	if (status != VFS_STATUS_OK) return status;

	ext4_set_le16(data->inode_buffer + 0x00U, inode->mode);
	ext4_set_le32(data->inode_buffer + 0x04U, (uint32_t)inode->size);
	ext4_set_le16(data->inode_buffer + 0x1AU, inode->links_count);
	ext4_set_le32(data->inode_buffer + 0x1CU, (uint32_t)inode->blocks_512);
	ext4_set_le32(data->inode_buffer + 0x20U, inode->flags);
	memcpy(data->inode_buffer + 0x28U, inode->block, sizeof(inode->block));
	ext4_set_le32(data->inode_buffer + 0x64U, inode->generation);
	ext4_set_le32(data->inode_buffer + 0x6CU, (uint32_t)(inode->size >> 32U));
	ext4_set_le16(data->inode_buffer + 0x74U, (uint16_t)(inode->blocks_512 >> 32U));

	return ext4_write_inode_raw_locked(data, inode->number, data->inode_buffer);
}

static void ext4_initialize_extent_root(ext4_inode_t *inode)
{
	memset(inode->block, 0, sizeof(inode->block));
	ext4_set_le16(inode->block + 0x00U, EXT4_EXTENT_MAGIC);
	ext4_set_le16(inode->block + 0x02U, 0U);
	ext4_set_le16(inode->block + 0x04U, EXT4_ROOT_EXTENT_CAPACITY);
	ext4_set_le16(inode->block + 0x06U, 0U);
	ext4_set_le32(inode->block + 0x08U, 0U);
}

/*
 * ext4_extent_node_valid:
 *
 * Validate the common extent header and ensure every advertised entry fits
 * inside the containing inode root or extent block.
 */
static bool ext4_extent_node_valid(const uint8_t *node, uint32_t capacity, uint16_t expected_depth)
{
	if (capacity < 12U) return false;

	if (ext4_le16(node + 0x00U) != EXT4_EXTENT_MAGIC) return false;

	uint16_t entries = ext4_le16(node + 0x02U);
	uint16_t maximum = ext4_le16(node + 0x04U);
	uint16_t depth = ext4_le16(node + 0x06U);

	if (depth != expected_depth || depth > EXT4_EXTENT_MAX_DEPTH) return false;

	if (entries > maximum) return false;
	if ((uint32_t)entries > (capacity - 12U) / 12U) return false;

	if ((uint32_t)maximum > (capacity - 12U) / 12U) return false;

	return true;
}

/*
 * ext4_map_block_locked:
 *
 * Resolve one logical file block through the inode extent tree. A missing
 * extent is reported as an unmapped sparse block. Uninitialized extents are
 * mapped but returned as zero-filled data by the read path.
 */
static vfs_status_t ext4_map_block_locked(
	ext4_mount_data_t *data,
	const ext4_inode_t *inode,
	uint32_t logical_block,
	bool *mapped,
	bool *initialized,
	uint64_t *physical_block
)
{
	if (mapped == 0 || initialized == 0 || physical_block == 0) return VFS_STATUS_INVALID;

	*mapped = false;
	*initialized = false;
	*physical_block = 0ULL;

	const uint8_t *node = inode->block;
	uint32_t capacity = sizeof(inode->block);
	uint16_t depth = ext4_le16(node + 0x06U);
	if (depth > EXT4_EXTENT_MAX_DEPTH) return VFS_STATUS_IO_ERROR;

	for (;;) {
		if (!ext4_extent_node_valid(node, capacity, depth)) return VFS_STATUS_IO_ERROR;

		uint16_t entries = ext4_le16(node + 0x02U);
		if (depth == 0U) {
			for (uint16_t index = 0U; index < entries; index++) {
				const uint8_t *extent = node + 12U + (uint32_t)index * 12U;
				uint32_t first = ext4_le32(extent + 0x00U);
				uint16_t encoded_length = ext4_le16(extent + 0x04U);
				if (encoded_length == 0U) return VFS_STATUS_IO_ERROR;

				bool extent_initialized = encoded_length <= 32768U;
				uint32_t length = extent_initialized
					? encoded_length
					: (uint32_t)encoded_length - 32768U;
				if (length == 0U) return VFS_STATUS_IO_ERROR;

				uint64_t end = (uint64_t)first + length;
				if (logical_block < first || logical_block >= end) continue;

				uint64_t start = ((uint64_t)ext4_le16(extent + 0x06U) << 32U)
					| ext4_le32(extent + 0x08U);
				uint64_t physical = start + ((uint64_t)logical_block - first);
				if (physical >= data->blocks_count) return VFS_STATUS_IO_ERROR;

				*mapped = true;
				*initialized = extent_initialized;
				*physical_block = physical;
				return VFS_STATUS_OK;
			}

			return VFS_STATUS_OK;
		}

		const uint8_t *selected = 0;
		for (uint16_t index = 0U; index < entries; index++) {
			const uint8_t *entry = node + 12U + (uint32_t)index * 12U;
			uint32_t first = ext4_le32(entry + 0x00U);
			if (first > logical_block) break;
			selected = entry;
		}

		if (selected == 0) return VFS_STATUS_OK;

		uint64_t child = ((uint64_t)ext4_le16(selected + 0x08U) << 32U)
			| ext4_le32(selected + 0x04U);
		if (child == 0ULL || child >= data->blocks_count) return VFS_STATUS_IO_ERROR;

		if (!ext4_read_block_locked(data, child, data->block_buffer)) return VFS_STATUS_IO_ERROR;

		if (!ext4_verify_extent_block_checksum(data, inode, data->block_buffer)) return VFS_STATUS_IO_ERROR;

		node = data->block_buffer;
		capacity = data->block_size - 4U;
		depth--;
	}
}


static uint16_t ext4_extent_entries(const ext4_inode_t *inode)
{
	return ext4_le16(inode->block + 0x02U);
}

static bool ext4_extent_root_writable(const ext4_inode_t *inode)
{
	return ext4_extent_node_valid(inode->block, sizeof(inode->block), 0U)
		&& ext4_le16(inode->block + 0x04U) == EXT4_ROOT_EXTENT_CAPACITY;
}

/*
 * ext4_extent_insert_root_locked:
 *
 * Insert one initialized block mapping into a depth-zero inode extent root.
 * Adjacent logical and physical blocks are merged whenever possible. The
 * writable foundation deliberately stops at the four extents resident in
 * inode.i_block; extent-tree node allocation belongs to a later scalability
 * pass and does not affect the VFS contract.
 */
static vfs_status_t ext4_extent_insert_root_locked(
	ext4_inode_t *inode,
	uint32_t logical,
	uint64_t physical
)
{
	if (!ext4_extent_root_writable(inode)) return VFS_STATUS_NOT_SUPPORTED;
	uint16_t entries = ext4_extent_entries(inode);

	for (uint16_t index = 0U; index < entries; index++) {
		uint8_t *extent = inode->block + 12U + (uint32_t)index * 12U;
		uint32_t first = ext4_le32(extent + 0x00U);
		uint16_t length = ext4_le16(extent + 0x04U);
		if (length == 0U || length > 32768U) return VFS_STATUS_NOT_SUPPORTED;
		uint64_t start = ((uint64_t)ext4_le16(extent + 0x06U) << 32U) | ext4_le32(extent + 0x08U);

		if (logical >= first && logical < (uint64_t)first + length) return VFS_STATUS_EXISTS;


		if ((uint64_t)first + length == logical && start + length == physical && length < 32768U) {
			ext4_set_le16(extent + 0x04U, (uint16_t)(length + 1U));
			return VFS_STATUS_OK;
		}

		if (logical + 1U == first && physical + 1ULL == start && length < 32768U) {
			ext4_set_le32(extent + 0x00U, logical);
			ext4_set_le16(extent + 0x04U, (uint16_t)(length + 1U));
			ext4_set_le16(extent + 0x06U, (uint16_t)(physical >> 32U));
			ext4_set_le32(extent + 0x08U, (uint32_t)physical);
			return VFS_STATUS_OK;
		}
	}

	if (entries >= EXT4_ROOT_EXTENT_CAPACITY) return VFS_STATUS_NO_SPACE;

	uint16_t insert = entries;
	for (uint16_t index = 0U; index < entries; index++) {
		uint8_t *extent = inode->block + 12U + (uint32_t)index * 12U;
		if (logical < ext4_le32(extent + 0x00U)) {
			insert = index;
			break;
		}
	}

	for (uint16_t index = entries; index > insert; index--) {
		memcpy(
			inode->block + 12U + (uint32_t)index * 12U,
			inode->block + 12U + (uint32_t)(index - 1U) * 12U,
			12U
		);
	}

	uint8_t *extent = inode->block + 12U + (uint32_t)insert * 12U;
	ext4_set_le32(extent + 0x00U, logical);
	ext4_set_le16(extent + 0x04U, 1U);
	ext4_set_le16(extent + 0x06U, (uint16_t)(physical >> 32U));
	ext4_set_le32(extent + 0x08U, (uint32_t)physical);
	ext4_set_le16(inode->block + 0x02U, (uint16_t)(entries + 1U));
	return VFS_STATUS_OK;
}

/*
 * ext4_extent_release_from_locked:
 *
 * Free all depth-zero extent blocks whose logical numbers are greater than
 * or equal to first_free. Extents crossing the boundary are shortened in
 * place. The number of released filesystem blocks is returned to the caller.
 */
static vfs_status_t ext4_extent_release_from_locked(
	ext4_mount_data_t *data,
	ext4_inode_t *inode,
	uint32_t first_free,
	uint32_t *released
)
{
	if (released == 0) return VFS_STATUS_INVALID;
	*released = 0U;
	if (!ext4_extent_root_writable(inode)) return VFS_STATUS_NOT_SUPPORTED;

	uint16_t entries = ext4_extent_entries(inode);
	uint16_t output = 0U;

	for (uint16_t index = 0U; index < entries; index++) {
		uint8_t current[12];
		memcpy(current, inode->block + 12U + (uint32_t)index * 12U, sizeof(current));

		uint32_t first = ext4_le32(current + 0x00U);
		uint16_t length = ext4_le16(current + 0x04U);
		if (length == 0U || length > 32768U) return VFS_STATUS_NOT_SUPPORTED;
		uint64_t start = ((uint64_t)ext4_le16(current + 0x06U) << 32U) | ext4_le32(current + 0x08U);
		uint64_t end = (uint64_t)first + length;

		if (end <= first_free) {
			memcpy(inode->block + 12U + (uint32_t)output * 12U, current, sizeof(current));
			output++;
			continue;
		}

		uint32_t keep = first < first_free ? first_free - first : 0U;
		if (keep > length) keep = length;
		for (uint32_t offset = keep; offset < length; offset++) {
			vfs_status_t status = ext4_free_block_locked(data, start + offset);
			if (status != VFS_STATUS_OK) return status;
			(*released)++;
		}

		if (keep != 0U) {
			ext4_set_le16(current + 0x04U, (uint16_t)keep);
			memcpy(inode->block + 12U + (uint32_t)output * 12U, current, sizeof(current));
			output++;
		}
	}

	for (uint16_t index = output; index < EXT4_ROOT_EXTENT_CAPACITY; index++) {
		memset(inode->block + 12U + (uint32_t)index * 12U, 0, 12U);
	}
	ext4_set_le16(inode->block + 0x02U, output);
	return VFS_STATUS_OK;
}

static uint32_t ext4_inode_group(ext4_mount_data_t *data, uint32_t inode_number)
{
	return (inode_number - 1U) / data->inodes_per_group;
}

/*
 * ext4_inode_ensure_block_locked:
 *
 * Return an initialized physical block for one logical file block. Sparse
 * holes are allocated on first write and added to the inode extent root.
 */
static vfs_status_t ext4_inode_ensure_block_locked(
	ext4_mount_data_t *data,
	ext4_inode_t *inode,
	uint32_t logical,
	uint64_t *physical,
	bool *new_block
)
{
	if (physical == 0 || new_block == 0) return VFS_STATUS_INVALID;
	*physical = 0ULL;
	*new_block = false;

	bool mapped;
	bool initialized;
	vfs_status_t status = ext4_map_block_locked(data, inode, logical, &mapped, &initialized, physical);
	if (status != VFS_STATUS_OK) return status;

	if (mapped) {
		if (!initialized) return VFS_STATUS_NOT_SUPPORTED;
		return VFS_STATUS_OK;
	}

	if (!ext4_extent_root_writable(inode)) return VFS_STATUS_NOT_SUPPORTED;
	uint64_t allocated;
	status = ext4_allocate_block_locked(data, ext4_inode_group(data, inode->number), &allocated);
	if (status != VFS_STATUS_OK) return status;

	status = ext4_extent_insert_root_locked(inode, logical, allocated);
	if (status != VFS_STATUS_OK) {
		(void)ext4_free_block_locked(data, allocated);
		return status;
	}

	inode->blocks_512 += data->block_size / BLOCK_SECTOR_SIZE;
	*physical = allocated;
	*new_block = true;
	return VFS_STATUS_OK;
}

/*
 * ext4_read_inode_data_locked:
 *
 * Read a byte range from an extent-backed inode. Sparse and uninitialized
 * extents read as zeroes. The caller holds the mount lock for the shared block
 * buffer for the entire operation.
 */
static vfs_status_t ext4_read_inode_data_locked(
	ext4_mount_data_t *data,
	const ext4_inode_t *inode,
	uint64_t offset,
	void *buffer,
	uint64_t size,
	uint64_t *read_size
)
{
	*read_size = 0ULL;
	if (size == 0ULL || offset >= inode->size) return VFS_STATUS_OK;

	uint64_t remaining = inode->size - offset;
	if (remaining > size) remaining = size;

	uint8_t *destination = buffer;

	while (remaining != 0ULL) {
		uint64_t logical64 = offset / data->block_size;
		if (logical64 > UINT32_MAX) return VFS_STATUS_NOT_SUPPORTED;

		uint32_t block_offset = (uint32_t)(offset % data->block_size);
		uint32_t available = data->block_size - block_offset;
		uint32_t amount = remaining < available ? (uint32_t)remaining : available;

		bool mapped;
		bool initialized;
		uint64_t physical;
		vfs_status_t status = ext4_map_block_locked(
			data,
			inode,
			(uint32_t)logical64,
			&mapped,
			&initialized,
			&physical
		);
		if (status != VFS_STATUS_OK) return status;

		if (!mapped || !initialized) {
			memset(destination, 0, amount);
		} else {
			if (!ext4_read_block_locked(data, physical, data->block_buffer)) return VFS_STATUS_IO_ERROR;
			memcpy(destination, data->block_buffer + block_offset, amount);
		}

		offset += amount;
		destination += amount;
		remaining -= amount;
		*read_size += amount;
	}

	return VFS_STATUS_OK;
}

static vnode_type_t ext4_vnode_type(uint16_t mode)
{
	switch (mode & EXT4_MODE_TYPE_MASK) {
	case EXT4_MODE_REGULAR:
		return VNODE_TYPE_REGULAR;
	case EXT4_MODE_DIRECTORY:
		return VNODE_TYPE_DIRECTORY;
	default:
		return VNODE_TYPE_NONE;
	}
}

static ext4_mount_data_t *ext4_data(vnode_t vnode)
{
	if (vnode == 0 || vnode->v_mount == 0) return 0;
	return vnode->v_mount->m_data;
}

static ext4_node_t *ext4_node(vnode_t vnode)
{
	if (vnode == 0) return 0;
	return vnode->v_data;
}

/*
 * ext4_get_node_locked:
 *
 * Return a resident vnode for one inode. New nodes are populated from the
 * on-disk inode table and remain cached until the mount is destroyed.
 */
static vfs_status_t ext4_get_node_locked(
	ext4_mount_data_t *data,
	uint32_t inode_number,
	vnode_t parent,
	vnode_t *result
)
{
	*result = 0;

	for (ext4_node_t *node = data->nodes; node != 0; node = node->next) {
		if (node->inode.number != inode_number) continue;

		if (node->vnode.v_parent == 0 && parent != 0) node->vnode.v_parent = parent;
		if (!vnode_reference(&node->vnode)) return VFS_STATUS_IO_ERROR;
		*result = &node->vnode;
		return VFS_STATUS_OK;
	}

	ext4_node_t *node = kcalloc(1U, sizeof(*node));
	if (node == 0) return VFS_STATUS_NO_MEMORY;

	vfs_status_t status = ext4_load_inode_locked(data, inode_number, &node->inode);
	if (status != VFS_STATUS_OK) {
		(void)kfree(node);
		return status;
	}

	vnode_type_t type = ext4_vnode_type(node->inode.mode);
	if (type == VNODE_TYPE_NONE) {
		(void)kfree(node);
		return VFS_STATUS_NOT_SUPPORTED;
	}

	vnode_init(
		&node->vnode,
		data->mount,
		parent,
		&g_ext4_vnode_operations,
		type,
		inode_number,
		node
	);
	node->vnode.v_size = node->inode.size;

	node->next = data->nodes;
	data->nodes = node;
	data->node_count++;

	if (!vnode_reference(&node->vnode)) {
		data->nodes = node->next;
		data->node_count--;
		(void)kfree(node);
		return VFS_STATUS_IO_ERROR;
	}

	*result = &node->vnode;
	return VFS_STATUS_OK;
}

static bool ext4_name_equal(const uint8_t *entry_name, uint32_t entry_length, const char *name)
{
	uint32_t name_length = 0U;
	while (name_length <= VFS_NAME_MAX && name[name_length] != '\0') name_length++;
	if (name_length > VFS_NAME_MAX || name_length != entry_length) return false;

	for (uint32_t index = 0U; index < entry_length; index++) {
		if (entry_name[index] != (uint8_t)name[index]) return false;
	}

	return true;
}


static void ext4_encode_inode(uint8_t *raw, const ext4_inode_t *inode)
{
	ext4_set_le16(raw + 0x00U, inode->mode);
	ext4_set_le32(raw + 0x04U, (uint32_t)inode->size);
	ext4_set_le16(raw + 0x1AU, inode->links_count);
	ext4_set_le32(raw + 0x1CU, (uint32_t)inode->blocks_512);
	ext4_set_le32(raw + 0x20U, inode->flags);
	memcpy(raw + 0x28U, inode->block, sizeof(inode->block));
	ext4_set_le32(raw + 0x64U, inode->generation);
	ext4_set_le32(raw + 0x6CU, (uint32_t)(inode->size >> 32U));
	ext4_set_le16(raw + 0x74U, (uint16_t)(inode->blocks_512 >> 32U));
}

static vfs_status_t ext4_store_new_inode_locked(ext4_mount_data_t *data, const ext4_inode_t *inode)
{
	memset(data->inode_buffer, 0, data->inode_size);
	ext4_encode_inode(data->inode_buffer, inode);
	if (data->inode_size >= 0xA0U) ext4_set_le16(data->inode_buffer + 0x80U, 32U);
	return ext4_write_inode_raw_locked(data, inode->number, data->inode_buffer);
}

static uint8_t ext4_directory_file_type(vnode_type_t type)
{
	return type == VNODE_TYPE_DIRECTORY ? EXT4_DIR_FT_DIRECTORY : EXT4_DIR_FT_REGULAR;
}

/*
 * ext4_directory_add_locked:
 *
 * Insert one directory entry into a classic linear directory. Space is taken
 * from an unused record or from slack at the end of an existing record. If no
 * existing block can hold the name, a new directory block is allocated and
 * appended through the inode extent root.
 */
static vfs_status_t ext4_directory_add_locked(
	ext4_mount_data_t *data,
	ext4_node_t *directory,
	const char *name,
	uint32_t inode_number,
	vnode_type_t type
)
{
	if ((directory->inode.flags & EXT4_INDEX_FL) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	uint32_t name_length = 0U;
	while (name_length <= VFS_NAME_MAX && name[name_length] != '\0') name_length++;
	if (name_length == 0U || name_length > VFS_NAME_MAX) return VFS_STATUS_NAME_TOO_LONG;
	uint32_t required = ext4_align4(8U + name_length);
	uint32_t limit = data->block_size - EXT4_DIRECTORY_TAIL_SIZE;
	uint64_t logical_blocks = (directory->inode.size + data->block_size - 1ULL) / data->block_size;
	for (uint64_t logical64 = 0ULL; logical64 < logical_blocks; logical64++) {
		if (logical64 > UINT32_MAX) return VFS_STATUS_NOT_SUPPORTED;
		bool mapped, initialized;
		uint64_t physical;
		vfs_status_t status = ext4_map_block_locked(data, &directory->inode, (uint32_t)logical64, &mapped, &initialized, &physical);
		if (status != VFS_STATUS_OK) return status;

		if (!mapped || !initialized) continue;
		if (!ext4_read_block_locked(data, physical, data->block_buffer)) return VFS_STATUS_IO_ERROR;

		if (!ext4_verify_directory_checksum(data, &directory->inode, data->block_buffer)) return VFS_STATUS_IO_ERROR;
		uint32_t offset = 0U;
		while (offset + 8U <= limit) {
			uint8_t *entry = data->block_buffer + offset;
			uint32_t entry_inode = ext4_le32(entry + 0x00U);
			uint16_t record_length = ext4_le16(entry + 0x04U);
			if (record_length < 8U || (record_length & 3U) != 0U || record_length > limit - offset) return VFS_STATUS_IO_ERROR;

			if (entry_inode == 0U && record_length >= required) {
				ext4_set_le32(entry + 0x00U, inode_number);
				entry[0x06U] = (uint8_t)name_length;
				entry[0x07U] = ext4_directory_file_type(type);
				memcpy(entry + 0x08U, name, name_length);
				ext4_set_directory_checksum(data, &directory->inode, data->block_buffer);
				return ext4_write_metadata_block_locked(data, physical, data->block_buffer) ? VFS_STATUS_OK : VFS_STATUS_IO_ERROR;
			}
			uint32_t minimum = ext4_align4(8U + entry[0x06U]);
			if (entry_inode != 0U && minimum <= record_length && record_length - minimum >= required) {
				uint16_t available = (uint16_t)(record_length - minimum);
				ext4_set_le16(entry + 0x04U, (uint16_t)minimum);
				uint8_t *created = entry + minimum;
				memset(created, 0, available);
				ext4_set_le32(created + 0x00U, inode_number);
				ext4_set_le16(created + 0x04U, available);
				created[0x06U] = (uint8_t)name_length;
				created[0x07U] = ext4_directory_file_type(type);
				memcpy(created + 0x08U, name, name_length);
				ext4_set_directory_checksum(data, &directory->inode, data->block_buffer);
				return ext4_write_metadata_block_locked(data, physical, data->block_buffer) ? VFS_STATUS_OK : VFS_STATUS_IO_ERROR;
			}
			offset += record_length;
		}
	}

	if (logical_blocks > UINT32_MAX) return VFS_STATUS_NOT_SUPPORTED;
	uint64_t physical;
	bool new_block;
	vfs_status_t status = ext4_inode_ensure_block_locked(data, &directory->inode, (uint32_t)logical_blocks, &physical, &new_block);
	if (status != VFS_STATUS_OK || !new_block) return status == VFS_STATUS_OK ? VFS_STATUS_IO_ERROR : status;
	memset(data->block_buffer, 0, data->block_size);
	ext4_set_le32(data->block_buffer + 0x00U, inode_number);
	ext4_set_le16(data->block_buffer + 0x04U, (uint16_t)limit);
	data->block_buffer[0x06U] = (uint8_t)name_length;
	data->block_buffer[0x07U] = ext4_directory_file_type(type);
	memcpy(data->block_buffer + 0x08U, name, name_length);
	ext4_set_directory_checksum(data, &directory->inode, data->block_buffer);
	if (!ext4_write_metadata_block_locked(data, physical, data->block_buffer)) return VFS_STATUS_IO_ERROR;
	directory->inode.size += data->block_size;
	directory->vnode.v_size = directory->inode.size;
	return ext4_store_inode_locked(data, &directory->inode);
}

/*
 * ext4_directory_remove_locked:
 *
 * Remove one named entry from a classic directory. The record is merged into
 * its predecessor when possible; a first entry is converted into an unused
 * record. Directory blocks are not compacted or released in this foundation.
 */
static vfs_status_t ext4_directory_remove_locked(
	ext4_mount_data_t *data,
	ext4_node_t *directory,
	const char *name,
	uint32_t *inode_number,
	vnode_type_t *type
)
{
	if (inode_number != 0) *inode_number = 0U;
	if (type != 0) *type = VNODE_TYPE_NONE;
	if ((directory->inode.flags & EXT4_INDEX_FL) != 0U) return VFS_STATUS_NOT_SUPPORTED;
	uint32_t limit = data->block_size - EXT4_DIRECTORY_TAIL_SIZE;
	uint64_t logical_blocks = (directory->inode.size + data->block_size - 1ULL) / data->block_size;
	for (uint64_t logical64 = 0ULL; logical64 < logical_blocks; logical64++) {
		if (logical64 > UINT32_MAX) return VFS_STATUS_NOT_SUPPORTED;
		bool mapped, initialized;
		uint64_t physical;
		vfs_status_t status = ext4_map_block_locked(data, &directory->inode, (uint32_t)logical64, &mapped, &initialized, &physical);
		if (status != VFS_STATUS_OK) return status;

		if (!mapped || !initialized) continue;
		if (!ext4_read_block_locked(data, physical, data->block_buffer)) return VFS_STATUS_IO_ERROR;

		if (!ext4_verify_directory_checksum(data, &directory->inode, data->block_buffer)) return VFS_STATUS_IO_ERROR;
		uint32_t offset = 0U;
		uint32_t previous = UINT32_MAX;
		while (offset + 8U <= limit) {
			uint8_t *entry = data->block_buffer + offset;
			uint32_t entry_inode = ext4_le32(entry + 0x00U);
			uint16_t record_length = ext4_le16(entry + 0x04U);
			if (record_length < 8U || (record_length & 3U) != 0U || record_length > limit - offset) return VFS_STATUS_IO_ERROR;
			uint32_t name_length = entry[0x06U];
			if (name_length > record_length - 8U) return VFS_STATUS_IO_ERROR;

			if (entry_inode != 0U && ext4_name_equal(entry + 0x08U, name_length, name)) {
				if (name_length == 1U && entry[0x08U] == '.') return VFS_STATUS_INVALID;

				if (name_length == 2U && entry[0x08U] == '.' && entry[0x09U] == '.') return VFS_STATUS_INVALID;
				if (inode_number != 0) *inode_number = entry_inode;
				if (type != 0) *type = entry[0x07U] == EXT4_DIR_FT_DIRECTORY ? VNODE_TYPE_DIRECTORY : VNODE_TYPE_REGULAR;
				if (previous != UINT32_MAX) {
					uint8_t *prev = data->block_buffer + previous;
					ext4_set_le16(prev + 0x04U, (uint16_t)(ext4_le16(prev + 0x04U) + record_length));
				} else {
					ext4_set_le32(entry + 0x00U, 0U);
				}
				ext4_set_directory_checksum(data, &directory->inode, data->block_buffer);
				return ext4_write_metadata_block_locked(data, physical, data->block_buffer) ? VFS_STATUS_OK : VFS_STATUS_IO_ERROR;
			}

			if (entry_inode != 0U) previous = offset;
			offset += record_length;
		}
	}
	return VFS_STATUS_NOT_FOUND;
}

static vfs_status_t ext4_initialize_directory_locked(
	ext4_mount_data_t *data,
	ext4_inode_t *inode,
	uint32_t parent_inode
)
{
	uint64_t physical;
	bool new_block;
	vfs_status_t status = ext4_inode_ensure_block_locked(data, inode, 0U, &physical, &new_block);
	if (status != VFS_STATUS_OK || !new_block) return status == VFS_STATUS_OK ? VFS_STATUS_IO_ERROR : status;
	uint32_t limit = data->block_size - EXT4_DIRECTORY_TAIL_SIZE;
	memset(data->block_buffer, 0, data->block_size);
	ext4_set_le32(data->block_buffer + 0x00U, inode->number);
	ext4_set_le16(data->block_buffer + 0x04U, 12U);
	data->block_buffer[0x06U] = 1U;
	data->block_buffer[0x07U] = EXT4_DIR_FT_DIRECTORY;
	data->block_buffer[0x08U] = '.';
	uint8_t *dotdot = data->block_buffer + 12U;
	ext4_set_le32(dotdot + 0x00U, parent_inode);
	ext4_set_le16(dotdot + 0x04U, (uint16_t)(limit - 12U));
	dotdot[0x06U] = 2U;
	dotdot[0x07U] = EXT4_DIR_FT_DIRECTORY;
	dotdot[0x08U] = '.';
	dotdot[0x09U] = '.';
	ext4_set_directory_checksum(data, inode, data->block_buffer);
	if (!ext4_write_metadata_block_locked(data, physical, data->block_buffer)) return VFS_STATUS_IO_ERROR;
	inode->size = data->block_size;
	return VFS_STATUS_OK;
}

static void ext4_remove_cached_node_locked(ext4_mount_data_t *data, uint32_t inode_number)
{
	ext4_node_t *previous = 0;
	ext4_node_t *node = data->nodes;
	while (node != 0) {
		if (node->inode.number == inode_number) {
			if (node->vnode.v_refcount != 1U) return;

			if (previous == 0) data->nodes = node->next;
			else previous->next = node->next;
			node->vnode.v_active = false;
			(void)kfree(node);
			if (data->node_count != 0U) data->node_count--;
			return;
		}
		previous = node;
		node = node->next;
	}
}

/*
 * ext4_lookup:
 *
 * Scan every data block of a directory and resolve one directory entry to an
 * inode-backed vnode. This works for classic directories and for indexed
 * directories because htree metadata blocks intentionally appear empty to a
 * linear reader while leaf blocks retain ordinary directory entries.
 */
static vfs_status_t ext4_lookup(vnode_t directory, const char *name, vnode_t *result)
{
	if (result != 0) *result = 0;
	if (directory == 0 || name == 0 || result == 0) return VFS_STATUS_INVALID;

	ext4_mount_data_t *data = ext4_data(directory);
	ext4_node_t *directory_node = ext4_node(directory);
	if (data == 0 || directory_node == 0) return VFS_STATUS_INVALID;

	ext4_lock(data);

	uint64_t logical_blocks = (directory_node->inode.size + data->block_size - 1ULL) / data->block_size;
	for (uint64_t logical64 = 0ULL; logical64 < logical_blocks; logical64++) {
		if (logical64 > UINT32_MAX) {
			ext4_unlock(data);
			return VFS_STATUS_NOT_SUPPORTED;
		}

		bool mapped;
		bool initialized;
		uint64_t physical;
		vfs_status_t status = ext4_map_block_locked(
			data,
			&directory_node->inode,
			(uint32_t)logical64,
			&mapped,
			&initialized,
			&physical
		);
		if (status != VFS_STATUS_OK) {
			ext4_unlock(data);
			return status;
		}

		if (!mapped || !initialized) continue;

		if (!ext4_read_block_locked(data, physical, data->block_buffer)) {
			ext4_unlock(data);
			return VFS_STATUS_IO_ERROR;
		}

		if (!ext4_verify_directory_checksum(data, &directory_node->inode, data->block_buffer)) {
			ext4_unlock(data);
			return VFS_STATUS_IO_ERROR;
		}

		uint64_t block_start = logical64 * data->block_size;
		uint64_t file_remaining = directory_node->inode.size - block_start;
		uint32_t block_limit = file_remaining < data->block_size
			? (uint32_t)file_remaining
			: data->block_size;
		if (block_limit == data->block_size) block_limit -= EXT4_DIRECTORY_TAIL_SIZE;

		uint32_t offset = 0U;
		while (offset + 8U <= block_limit) {
			const uint8_t *entry = data->block_buffer + offset;
			uint32_t inode_number = ext4_le32(entry + 0x00U);
			uint16_t record_length = ext4_le16(entry + 0x04U);

			if (record_length < 8U || (record_length & 3U) != 0U) {
				ext4_unlock(data);
				return VFS_STATUS_IO_ERROR;
			}

			if (record_length > block_limit - offset) {
				ext4_unlock(data);
				return VFS_STATUS_IO_ERROR;
			}

			uint32_t name_length = (data->feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0U
				? entry[0x06U]
				: ext4_le16(entry + 0x06U);
			if (name_length > record_length - 8U) {
				ext4_unlock(data);
				return VFS_STATUS_IO_ERROR;
			}

			if (inode_number != 0U && ext4_name_equal(entry + 0x08U, name_length, name)) {
				status = ext4_get_node_locked(data, inode_number, directory, result);
				ext4_unlock(data);
				return status;
			}

			offset += record_length;
		}
	}

	ext4_unlock(data);
	return VFS_STATUS_NOT_FOUND;
}

/*
 * Routine:     ext4_readdir
 * Purpose:
 *              Walk ext4 directory records using the open-file byte offset
 *              as the enumeration cookie. Htree metadata records have inode
 *              zero and are skipped like the linear lookup path.
 */
static vfs_status_t
ext4_readdir(vnode_t directory, uint64_t *offset, vfs_dirent_t *result)
{
	if (directory == 0 || offset == 0 || result == 0) return VFS_STATUS_INVALID;

	ext4_mount_data_t *data = ext4_data(directory);
	ext4_node_t *directory_node = ext4_node(directory);
	if (data == 0 || directory_node == 0) return VFS_STATUS_INVALID;

	ext4_lock(data);

	while (*offset < directory_node->inode.size) {
		uint64_t logical64 = *offset / data->block_size;
		if (logical64 > UINT32_MAX) {
			ext4_unlock(data);
			return VFS_STATUS_NOT_SUPPORTED;
		}

		bool mapped;
		bool initialized;
		uint64_t physical;
		vfs_status_t status = ext4_map_block_locked(
			data,
			&directory_node->inode,
			(uint32_t)logical64,
			&mapped,
			&initialized,
			&physical
		);
		if (status != VFS_STATUS_OK) {
			ext4_unlock(data);
			return status;
		}

		uint64_t block_start = logical64 * data->block_size;
		if (!mapped || !initialized) {
			*offset = block_start + data->block_size;
			continue;
		}

		if (!ext4_read_block_locked(data, physical, data->block_buffer)) {
			ext4_unlock(data);
			return VFS_STATUS_IO_ERROR;
		}

		if (!ext4_verify_directory_checksum(data, &directory_node->inode, data->block_buffer)) {
			ext4_unlock(data);
			return VFS_STATUS_IO_ERROR;
		}

		uint64_t file_remaining = directory_node->inode.size - block_start;
		uint32_t block_limit = file_remaining < data->block_size
			? (uint32_t)file_remaining
			: data->block_size;
		if (block_limit == data->block_size) block_limit -= EXT4_DIRECTORY_TAIL_SIZE;

		uint32_t block_offset = (uint32_t)(*offset - block_start);
		if (block_offset >= block_limit) {
			*offset = block_start + data->block_size;
			continue;
		}

		if (block_offset + 8U > block_limit) {
			ext4_unlock(data);
			return VFS_STATUS_IO_ERROR;
		}

		const uint8_t *entry = data->block_buffer + block_offset;
		uint32_t inode_number = ext4_le32(entry + 0x00U);
		uint16_t record_length = ext4_le16(entry + 0x04U);
		if (record_length < 8U || (record_length & 3U) != 0U || record_length > block_limit - block_offset) {
			ext4_unlock(data);
			return VFS_STATUS_IO_ERROR;
		}

		uint32_t name_length = (data->feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0U
			? entry[0x06U]
			: ext4_le16(entry + 0x06U);
		if (name_length > record_length - 8U || name_length > VFS_DIRENT_NAME_MAX) {
			ext4_unlock(data);
			return VFS_STATUS_IO_ERROR;
		}

		*offset += record_length;
		if (inode_number == 0U) continue;

		for (uint32_t index = 0U; index < name_length; index++) result->name[index] = (char)entry[0x08U + index];
		result->name[name_length] = '\0';
		result->inode = inode_number;
		result->name_length = name_length;
		result->type = VNODE_TYPE_NONE;

		if ((data->feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0U) {
			if (entry[0x07U] == EXT4_DIR_FT_REGULAR) result->type = VNODE_TYPE_REGULAR;
			if (entry[0x07U] == EXT4_DIR_FT_DIRECTORY) result->type = VNODE_TYPE_DIRECTORY;
		}

		ext4_unlock(data);
		return VFS_STATUS_OK;
	}

	ext4_unlock(data);
	return VFS_STATUS_END_OF_DIRECTORY;
}

static vfs_status_t ext4_create(
	vnode_t directory,
	const char *name,
	vnode_type_t type,
	vnode_t *result
)
{
	if (result != 0) *result = 0;
	if (directory == 0 || name == 0 || result == 0) return VFS_STATUS_INVALID;

	if (type != VNODE_TYPE_REGULAR && type != VNODE_TYPE_DIRECTORY) return VFS_STATUS_NOT_SUPPORTED;
	ext4_mount_data_t *data = ext4_data(directory);
	ext4_node_t *parent = ext4_node(directory);
	if (data == 0 || parent == 0) return VFS_STATUS_INVALID;

	if ((parent->inode.flags & EXT4_INDEX_FL) != 0U) return VFS_STATUS_NOT_SUPPORTED;

	ext4_lock(data);
	ext4_inode_t original_parent = parent->inode;
	uint64_t original_free_blocks = data->free_blocks_count;
	uint32_t original_free_inodes = data->free_inodes_count;
	vfs_status_t status = ext4_transaction_begin_locked(data);
	if (status != VFS_STATUS_OK) { ext4_unlock(data); return status; }

	uint32_t inode_number = 0U;
	status = ext4_allocate_inode_locked(data, type == VNODE_TYPE_DIRECTORY, ext4_inode_group(data, parent->inode.number), &inode_number);
	if (status != VFS_STATUS_OK) goto fail;
	ext4_inode_t inode;
	memset(&inode, 0, sizeof(inode));
	inode.number = inode_number;
	inode.mode = type == VNODE_TYPE_DIRECTORY ? EXT4_INODE_MODE_DIRECTORY : EXT4_INODE_MODE_REGULAR;
	inode.flags = EXT4_EXTENTS_FL;
	inode.generation = data->next_generation++;
	if (data->next_generation == 0U) data->next_generation = 1U;
	inode.links_count = type == VNODE_TYPE_DIRECTORY ? 2U : 1U;
	ext4_initialize_extent_root(&inode);
	status = ext4_store_new_inode_locked(data, &inode);
	if (status != VFS_STATUS_OK) goto fail;
	if (type == VNODE_TYPE_DIRECTORY) {
		status = ext4_initialize_directory_locked(data, &inode, parent->inode.number);
		if (status != VFS_STATUS_OK) goto fail;
		status = ext4_store_inode_locked(data, &inode);
		if (status != VFS_STATUS_OK) goto fail;
	}
	status = ext4_directory_add_locked(data, parent, name, inode_number, type);
	if (status != VFS_STATUS_OK) goto fail;
	if (type == VNODE_TYPE_DIRECTORY) {
		if (parent->inode.links_count != UINT16_MAX) parent->inode.links_count++;
		status = ext4_store_inode_locked(data, &parent->inode);
		if (status != VFS_STATUS_OK) goto fail;
	}
	status = ext4_get_node_locked(data, inode_number, directory, result);
	if (status != VFS_STATUS_OK) goto fail;
	status = ext4_transaction_commit_locked(data);
	if (status == VFS_STATUS_OK) { ext4_unlock(data); return VFS_STATUS_OK; }
	vnode_rele(*result);
	*result = 0;
	ext4_remove_cached_node_locked(data, inode_number);
	goto commit_fail;

fail:
	ext4_transaction_abort_locked(data);
commit_fail:
	parent->inode = original_parent;
	data->free_blocks_count = original_free_blocks;
	data->free_inodes_count = original_free_inodes;
	ext4_unlock(data);
	return status;
}

/*
 * ext4_unlink:
 *
 * Remove one regular-file name. Open-file deferred deletion is not yet part
 * of the VFS lifetime model, so a cached vnode with active references is
 * reported busy instead of being orphaned.
 */
static vfs_status_t ext4_unlink(vnode_t directory, const char *name)
{
	if (directory == 0 || name == 0) return VFS_STATUS_INVALID;
	vnode_t target;
	vfs_status_t status = ext4_lookup(directory, name, &target);
	if (status != VFS_STATUS_OK) return status;

	if (target->v_type == VNODE_TYPE_DIRECTORY) { vnode_rele(target); return VFS_STATUS_IS_DIRECTORY; }
	if (target->v_refcount > 2U) { vnode_rele(target); return VFS_STATUS_BUSY; }
	uint32_t expected_inode = (uint32_t)target->v_id;
	vnode_rele(target);
	ext4_mount_data_t *data = ext4_data(directory);
	ext4_node_t *parent = ext4_node(directory);
	if (data == 0 || parent == 0) return VFS_STATUS_INVALID;

	ext4_lock(data);
	uint64_t original_free_blocks = data->free_blocks_count;
	uint32_t original_free_inodes = data->free_inodes_count;
	status = ext4_transaction_begin_locked(data);
	if (status != VFS_STATUS_OK) { ext4_unlock(data); return status; }
	uint32_t inode_number;
	vnode_type_t type;
	status = ext4_directory_remove_locked(data, parent, name, &inode_number, &type);
	if (status != VFS_STATUS_OK) goto fail;
	if (type == VNODE_TYPE_DIRECTORY || inode_number != expected_inode) { status = VFS_STATUS_IO_ERROR; goto fail; }
	ext4_inode_t inode;
	status = ext4_load_inode_locked(data, inode_number, &inode);
	if (status != VFS_STATUS_OK) goto fail;
	if (inode.links_count > 1U) {
		inode.links_count--;
		status = ext4_store_inode_locked(data, &inode);
	} else {
		uint32_t released;
		status = ext4_extent_release_from_locked(data, &inode, 0U, &released);
		if (status == VFS_STATUS_OK) status = ext4_free_inode_locked(data, inode_number, false);
	}

	if (status != VFS_STATUS_OK) goto fail;
	status = ext4_transaction_commit_locked(data);
	if (status == VFS_STATUS_OK) ext4_remove_cached_node_locked(data, inode_number);
	ext4_unlock(data);
	return status;
fail:
	data->free_blocks_count = original_free_blocks;
	data->free_inodes_count = original_free_inodes;
	ext4_transaction_abort_locked(data);
	ext4_unlock(data);
	return status;
}

static vfs_status_t ext4_read(
	vnode_t vnode,
	uint64_t offset,
	void *buffer,
	uint64_t size,
	uint64_t *read_size
)
{
	if (read_size != 0) *read_size = 0ULL;

	ext4_mount_data_t *data = ext4_data(vnode);
	ext4_node_t *node = ext4_node(vnode);
	if (data == 0 || node == 0 || buffer == 0 || read_size == 0) return VFS_STATUS_INVALID;

	if (vnode->v_type != VNODE_TYPE_REGULAR) return VFS_STATUS_NOT_SUPPORTED;

	ext4_lock(data);
	vfs_status_t status = ext4_read_inode_data_locked(data, &node->inode, offset, buffer, size, read_size);
	ext4_unlock(data);
	return status;
}

static vfs_status_t ext4_write(
	vnode_t vnode,
	uint64_t offset,
	const void *buffer,
	uint64_t size,
	uint64_t *written_size
)
{
	if (written_size != 0) *written_size = 0ULL;
	if (vnode == 0 || buffer == 0 || written_size == 0) return VFS_STATUS_INVALID;

	if (vnode->v_type != VNODE_TYPE_REGULAR) return VFS_STATUS_NOT_SUPPORTED;
	if (size == 0ULL) return VFS_STATUS_OK;

	if (size > UINT64_MAX - offset) return VFS_STATUS_NO_SPACE;
	ext4_mount_data_t *data = ext4_data(vnode);
	ext4_node_t *node = ext4_node(vnode);
	if (data == 0 || node == 0) return VFS_STATUS_INVALID;

	if (!ext4_extent_root_writable(&node->inode)) return VFS_STATUS_NOT_SUPPORTED;
	uint64_t end = offset + size;
	if ((end - 1ULL) / data->block_size > UINT32_MAX) return VFS_STATUS_NOT_SUPPORTED;

	ext4_lock(data);
	ext4_inode_t original_inode = node->inode;
	uint64_t original_free_blocks = data->free_blocks_count;
	vfs_status_t status = ext4_transaction_begin_locked(data);
	if (status != VFS_STATUS_OK) { ext4_unlock(data); return status; }
	const uint8_t *source = buffer;
	uint64_t remaining = size;
	uint64_t cursor = offset;
	while (remaining != 0ULL) {
		uint32_t logical = (uint32_t)(cursor / data->block_size);
		uint32_t block_offset = (uint32_t)(cursor % data->block_size);
		uint32_t available = data->block_size - block_offset;
		uint32_t amount = remaining < available ? (uint32_t)remaining : available;
		uint64_t physical;
		bool new_block;
		status = ext4_inode_ensure_block_locked(data, &node->inode, logical, &physical, &new_block);
		if (status != VFS_STATUS_OK) goto fail;
		if (new_block) memset(data->block_buffer, 0, data->block_size);
		else if (block_offset != 0U || amount != data->block_size) {
			if (!ext4_read_block_locked(data, physical, data->block_buffer)) { status = VFS_STATUS_IO_ERROR; goto fail; }
		}

		if (block_offset == 0U && amount == data->block_size) {
			if (!ext4_write_block_locked(data, physical, source)) { status = VFS_STATUS_IO_ERROR; goto fail; }
		} else {
			memcpy(data->block_buffer + block_offset, source, amount);
			if (!ext4_write_block_locked(data, physical, data->block_buffer)) { status = VFS_STATUS_IO_ERROR; goto fail; }
		}
		cursor += amount;
		source += amount;
		remaining -= amount;
		*written_size += amount;
	}

	if (end > node->inode.size) node->inode.size = end;
	status = ext4_store_inode_locked(data, &node->inode);
	if (status != VFS_STATUS_OK) goto fail;
	status = ext4_transaction_commit_locked(data);
	if (status == VFS_STATUS_OK) {
		vnode->v_size = node->inode.size;
		ext4_unlock(data);
		return VFS_STATUS_OK;
	}
	goto commit_fail;
fail:
	ext4_transaction_abort_locked(data);
commit_fail:
	node->inode = original_inode;
	vnode->v_size = original_inode.size;
	data->free_blocks_count = original_free_blocks;
	*written_size = 0ULL;
	ext4_unlock(data);
	return status;
}

static vfs_status_t ext4_truncate(vnode_t vnode, uint64_t size)
{
	if (vnode == 0 || vnode->v_type != VNODE_TYPE_REGULAR) return VFS_STATUS_INVALID;
	ext4_mount_data_t *data = ext4_data(vnode);
	ext4_node_t *node = ext4_node(vnode);
	if (data == 0 || node == 0) return VFS_STATUS_INVALID;

	if (!ext4_extent_root_writable(&node->inode)) return VFS_STATUS_NOT_SUPPORTED;
	if (size == node->inode.size) return VFS_STATUS_OK;

	ext4_lock(data);
	ext4_inode_t original_inode = node->inode;
	uint64_t original_free_blocks = data->free_blocks_count;
	vfs_status_t status = ext4_transaction_begin_locked(data);
	if (status != VFS_STATUS_OK) { ext4_unlock(data); return status; }
	if (size < node->inode.size) {
		uint32_t keep_blocks = (uint32_t)((size + data->block_size - 1ULL) / data->block_size);
		if (size != 0ULL && (size % data->block_size) != 0ULL) {
			uint32_t logical = (uint32_t)(size / data->block_size);
			bool mapped, initialized;
			uint64_t physical;
			status = ext4_map_block_locked(data, &node->inode, logical, &mapped, &initialized, &physical);
			if (status != VFS_STATUS_OK) goto fail;
			if (mapped && initialized) {
				if (!ext4_read_block_locked(data, physical, data->block_buffer)) { status = VFS_STATUS_IO_ERROR; goto fail; }
				uint32_t start = (uint32_t)(size % data->block_size);
				memset(data->block_buffer + start, 0, data->block_size - start);
				if (!ext4_write_block_locked(data, physical, data->block_buffer)) { status = VFS_STATUS_IO_ERROR; goto fail; }
			}
		}
		uint32_t released;
		status = ext4_extent_release_from_locked(data, &node->inode, keep_blocks, &released);
		if (status != VFS_STATUS_OK) goto fail;
		uint64_t sectors = (uint64_t)released * (data->block_size / BLOCK_SECTOR_SIZE);
		if (sectors > node->inode.blocks_512) node->inode.blocks_512 = 0ULL;
		else node->inode.blocks_512 -= sectors;
	}
	node->inode.size = size;
	status = ext4_store_inode_locked(data, &node->inode);
	if (status != VFS_STATUS_OK) goto fail;
	status = ext4_transaction_commit_locked(data);
	if (status == VFS_STATUS_OK) {
		vnode->v_size = size;
		ext4_unlock(data);
		return VFS_STATUS_OK;
	}
	goto commit_fail;
fail:
	ext4_transaction_abort_locked(data);
commit_fail:
	node->inode = original_inode;
	vnode->v_size = original_inode.size;
	data->free_blocks_count = original_free_blocks;
	ext4_unlock(data);
	return status;
}

/*
 * ext4_destroy_mount_data:
 *
 * Release private storage for a mount which failed before publication. No
 * vnode may be externally referenced when this routine is used.
 */
/*
 * ext4_journal_map_locked:
 *
 * Resolve every logical block of the internal journal through the journal
 * inode extent tree. The map is retained by JBD2 so log I/O does not depend
 * on the journal file being physically contiguous.
 */
static vfs_status_t ext4_journal_map_locked(
	ext4_mount_data_t *data,
	uint64_t **block_map,
	uint32_t *block_count
)
{
	if (block_map == 0 || block_count == 0) return VFS_STATUS_INVALID;
	*block_map = 0;
	*block_count = 0U;

	ext4_inode_t inode;
	vfs_status_t status = ext4_load_inode_locked(data, data->journal_inode, &inode);
	if (status != VFS_STATUS_OK) return status;

	if ((inode.mode & EXT4_MODE_TYPE_MASK) != EXT4_MODE_REGULAR) return VFS_STATUS_NOT_SUPPORTED;
	if (inode.size == 0ULL || (inode.size % data->block_size) != 0ULL) return VFS_STATUS_IO_ERROR;

	uint64_t blocks64 = inode.size / data->block_size;
	if (blocks64 == 0ULL || blocks64 > UINT32_MAX) return VFS_STATUS_NOT_SUPPORTED;

	uint32_t blocks = (uint32_t)blocks64;
	uint64_t *map = kmalloc((size_t)blocks * sizeof(*map));
	if (map == 0) return VFS_STATUS_NO_MEMORY;

	uint32_t runs = 0U;
	uint64_t previous = 0ULL;

	for (uint32_t logical = 0U; logical < blocks; logical++) {
		bool mapped;
		bool initialized;
		uint64_t physical;

		status = ext4_map_block_locked(
			data,
			&inode,
			logical,
			&mapped,
			&initialized,
			&physical
		);

		if (status != VFS_STATUS_OK || !mapped || !initialized) {
			(void)kfree(map);
			return status == VFS_STATUS_OK ? VFS_STATUS_IO_ERROR : status;
		}

		if (logical == 0U || physical != previous + 1ULL) runs++;
		map[logical] = physical;
		previous = physical;
	}

	kputs("IOFilesystemFamily: journal blocks ");
	kputu64(blocks);
	kputs(", physical runs ");
	kputu64(runs);
	kputc('\n');

	*block_map = map;
	*block_count = blocks;
	return VFS_STATUS_OK;
}

static vfs_status_t ext4_recover_journal_locked(ext4_mount_data_t *data)
{
	jbd2_status_t js = jbd2_recover(&data->journal);
	if (js != JBD2_STATUS_OK) return ext4_jbd2_status(js);
	uint8_t super[EXT4_SUPERBLOCK_SIZE];
	if (!ext4_read_bytes(data, EXT4_SUPERBLOCK_OFFSET, super, sizeof(super))) return VFS_STATUS_IO_ERROR;

	if (ext4_superblock_checksum(super) != ext4_le32(super + 0x3FCU)) return VFS_STATUS_IO_ERROR;
	memcpy(data->superblock, super, sizeof(data->superblock));
	data->feature_incompat = ext4_le32(super + 0x60U);
	data->free_inodes_count = ext4_le32(super + 0x10U);
	data->free_blocks_count = ext4_le32(super + 0x0CU);
	if ((data->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0U) data->free_blocks_count |= (uint64_t)ext4_le32(super + 0x158U) << 32U;
	return ext4_set_recovery_flag_locked(data, false);
}

static void ext4_destroy_mount_data(ext4_mount_data_t *data)
{
	if (data == 0) return;

	ext4_node_t *node = data->nodes;
	while (node != 0) {
		ext4_node_t *next = node->next;
		(void)kfree(node);
		node = next;
	}

	jbd2_destroy(&data->journal);
	if (data->inode_buffer != 0) (void)kfree(data->inode_buffer);
	if (data->block_buffer != 0) (void)kfree(data->block_buffer);
	(void)kfree(data);
}

/*
 * ext4_mount:
 *
 * Attach one writable ext4 volume with metadata_csum and an internal JBD2
 * journal. Recovery is completed before inode 2 is published as the mount
 * root, so no vnode can observe metadata from an uncheckpointed transaction.
 */
static vfs_status_t ext4_mount(
	filesystem_t filesystem,
	block_device_t device,
	mount_t mount
)
{
	(void)filesystem;
	if (device == 0 || mount == 0 || !device->registered) return VFS_STATUS_INVALID;

	if (g_ext4_mount_count >= EXT4_MOUNT_MAX) return VFS_STATUS_NO_SPACE;

	ext4_mount_data_t *data = kcalloc(1U, sizeof(*data));
	if (data == 0) return VFS_STATUS_NO_MEMORY;

	data->mount = mount;
	data->device = device;
	mount->m_data = data;

	uint8_t super[EXT4_SUPERBLOCK_SIZE];
	if (!ext4_read_bytes(data, EXT4_SUPERBLOCK_OFFSET, super, sizeof(super))) {
		mount->m_data = 0;
		ext4_destroy_mount_data(data);
		return VFS_STATUS_IO_ERROR;
	}

	vfs_status_t status = ext4_parse_superblock(data, super);
	if (status != VFS_STATUS_OK) {
		mount->m_data = 0;
		ext4_destroy_mount_data(data);
		return status;
	}

	data->block_buffer = kmalloc(data->block_size);
	data->inode_buffer = kmalloc(data->inode_size);
	if (data->block_buffer == 0 || data->inode_buffer == 0) {
		mount->m_data = 0;
		ext4_destroy_mount_data(data);
		return VFS_STATUS_NO_MEMORY;
	}
	memset(data->block_buffer, 0, data->block_size);

	ext4_lock(data);
	uint64_t *journal_map = 0;
	uint32_t journal_blocks;
	status = ext4_journal_map_locked(data, &journal_map, &journal_blocks);
	if (status == VFS_STATUS_OK) {
		status = ext4_jbd2_status(jbd2_init(
			&data->journal, data->device, data->block_size, data->blocks_count,
			journal_map, journal_blocks, data->uuid
		));
	}

	if (journal_map != 0) (void)kfree(journal_map);
	if (status == VFS_STATUS_OK) status = ext4_recover_journal_locked(data);
	if (status != VFS_STATUS_OK) {
		ext4_unlock(data);
		mount->m_data = 0;
		ext4_destroy_mount_data(data);
		return status;
	}
	vnode_t root;
	status = ext4_get_node_locked(data, EXT4_ROOT_INODE, 0, &root);
	ext4_unlock(data);
	if (status != VFS_STATUS_OK || root == 0 || root->v_type != VNODE_TYPE_DIRECTORY) {
		mount->m_data = 0;
		ext4_destroy_mount_data(data);
		return status == VFS_STATUS_OK ? VFS_STATUS_IO_ERROR : status;
	}

	mount->m_root = root;
	data->active = true;
	g_ext4_mounts[g_ext4_mount_count++] = data;
	return VFS_STATUS_OK;
}

/*
 * ext4_sync:
 *
 * Every metadata mutation is checkpointed before its VFS operation returns.
 * Sync therefore validates that no transaction is in flight and asks the
 * block layer to commit any volatile device cache.
 */
static vfs_status_t ext4_space_info(mount_t mount, vfs_space_info_t *info)
{
	if (mount == 0 || info == 0) return VFS_STATUS_INVALID;
	ext4_mount_data_t *data = mount->m_data;
	if (data == 0 || !data->active) return VFS_STATUS_INVALID;

	ext4_lock(data);
	*info = (vfs_space_info_t) {
		.total_bytes = data->blocks_count * (uint64_t)data->block_size,
		.free_bytes = data->free_blocks_count * (uint64_t)data->block_size,
		.block_size = data->block_size,
		.read_only = data->device != 0 && data->device->read_only
	};
	ext4_unlock(data);
	return VFS_STATUS_OK;
}

static vfs_status_t ext4_sync(mount_t mount)
{
	if (mount == 0 || mount->m_data == 0) return VFS_STATUS_INVALID;

	ext4_mount_data_t *data = mount->m_data;
	ext4_lock(data);

	if (!data->active || jbd2_is_transaction_active(&data->journal)) {
		ext4_unlock(data);
		return VFS_STATUS_BUSY;
	}

	if (data->recovery_required) {
		ext4_unlock(data);
		return VFS_STATUS_IO_ERROR;
	}

	bool flushed = block_device_flush(data->device);
	ext4_unlock(data);
	return flushed ? VFS_STATUS_OK : VFS_STATUS_IO_ERROR;
}

/*
 * ext4_unmount:
 *
 * Tear down one clean ext4 mount. Cached vnodes each retain one filesystem
 * residency reference; the root retains one additional mount reference.
 * Any other reference means a pathname or open file still owns the vnode and
 * unmount must fail with BUSY.
 */
static vfs_status_t ext4_unmount(mount_t mount)
{
	if (mount == 0 || mount->m_data == 0 || mount->m_root == 0) return VFS_STATUS_INVALID;

	ext4_mount_data_t *data = mount->m_data;
	vfs_status_t status = ext4_sync(mount);
	if (status != VFS_STATUS_OK) return status;

	ext4_lock(data);
	for (ext4_node_t *node = data->nodes; node != 0; node = node->next) {
		uint32_t expected = &node->vnode == mount->m_root ? 2U : 1U;
		if (node->vnode.v_refcount != expected) {
			ext4_unlock(data);
			return VFS_STATUS_BUSY;
		}
	}

	for (ext4_node_t *node = data->nodes; node != 0; node = node->next) {
		node->vnode.v_active = false;
	}
	data->active = false;
	ext4_unlock(data);

	for (uint32_t index = 0U; index < g_ext4_mount_count; index++) {
		if (g_ext4_mounts[index] != data) continue;
		for (uint32_t move = index + 1U; move < g_ext4_mount_count; move++) {
			g_ext4_mounts[move - 1U] = g_ext4_mounts[move];
		}
		g_ext4_mount_count--;
		g_ext4_mounts[g_ext4_mount_count] = 0;
		break;
	}

	mount->m_data = 0;
	mount->m_root = 0;
	ext4_destroy_mount_data(data);
	return VFS_STATUS_OK;
}

bool ext4_register(void)
{
	return vfs_register_filesystem(&g_ext4_filesystem);
}

uint32_t ext4_mount_count(void)
{
	return g_ext4_mount_count;
}

bool ext4_debug_arm_journal_crash(void)
{
	for (uint32_t index = 0U; index < g_ext4_mount_count; index++) {
		ext4_mount_data_t *data = g_ext4_mounts[index];
		if (data == 0 || !data->active || !data->journal.active) continue;
		jbd2_set_crash_after_commit(&data->journal, true);
		return true;
	}
	return false;
}

bool ext4_debug_journal_crash_reached(void)
{
	for (uint32_t index = 0U; index < g_ext4_mount_count; index++) {
		ext4_mount_data_t *data = g_ext4_mounts[index];
		if (data == 0 || !data->active || !data->journal.active) continue;
		return jbd2_crash_point_reached(&data->journal);
	}
	return false;
}

/*
 * ext4_dump:
 *
 * Print stable geometry and feature state for every attached ext4 volume.
 */
void ext4_dump(void)
{
	for (uint32_t index = 0U; index < g_ext4_mount_count; index++) {
		ext4_mount_data_t *data = g_ext4_mounts[index];
		if (data == 0 || !data->active) continue;

		kputs("IOFilesystemFamily: mount ");
		kputs(data->mount->m_path);
		kputs(", block size: ");
		kputu64(data->block_size);
		kputs(", blocks: ");
		kputu64(data->blocks_count);
		kputs(", groups: ");
		kputu64(data->group_count);
		kputs(", inode size: ");
		kputu64(data->inode_size);
		kputc('\n');

		kputs("IOFilesystemFamily: features compat ");
		kputhex32(data->feature_compat);
		kputs(", incompat ");
		kputhex32(data->feature_incompat);
		kputs(", ro-compat ");
		kputhex32(data->feature_ro_compat);
		kputc('\n');

		kputs("IOFilesystemFamily: resident vnodes: ");
		kputu64(data->node_count);
		kputc('\n');

		kputs("IOFilesystemFamily: free blocks: ");
		kputu64(data->free_blocks_count);
		kputs(", free inodes: ");
		kputu64(data->free_inodes_count);
		kputc('\n');

		kputln("IOFilesystemFamily: metadata checksums: CRC32C");
		kputln("IOFilesystemFamily: journal: JBD2 checksum-v3, ordered data");
		kputs("jbd2: commits: ");
		kputu64(data->journal.commit_count);
		kputs(", replays: ");
		kputu64(data->journal.replay_count);
		kputs(", replayed metadata blocks: ");
		kputu64(data->journal.replayed_block_count);
		kputs(", discarded incomplete transactions: ");
		kputu64(data->journal.discard_count);
		kputc('\n');
		if (data->journal.replay_count != 0ULL) {
			kputs("jbd2: last replay sequence: ");
			kputu64(data->journal.last_replay_sequence);
			kputs(", metadata blocks: ");
			kputu64(data->journal.last_replay_blocks);
			kputc('\n');
		}
	}
}
