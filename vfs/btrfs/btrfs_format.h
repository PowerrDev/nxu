/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_format.h
 *
 * Btrfs on-disk format: constants, keys, status codes, explicit little-endian
 * field accessors and the parsed (host order) forms of every structure the
 * read-only driver looks at.
 *
 * Rules that the rest of the driver relies on:
 *
 *   - All on-disk data is little-endian and is read through the btrfs_get_*
 *     byte accessors. A byte buffer is NEVER cast to a structure pointer: the
 *     buffers are 1-byte aligned at arbitrary offsets and strict aliasing
 *     forbids it, on arm64 and on i386 alike.
 *   - Every parse function is given the number of bytes actually available
 *     and fails (returns false) instead of reading past them. Callers pass
 *     the size of the item or block the bytes came from, so a corrupt length
 *     can never turn into an out-of-bounds read.
 *   - This header (and the whole core under vfs/btrfs/ except btrfs_vfs.c,
 *     btrfs_io_block.c and btrfs_io_host.c) includes no kernel headers and
 *     compiles for the host with the system headers.
 *
 * References: include/uapi/linux/btrfs_tree.h and btrfs.h in the Linux
 * kernel, and the "On-disk format" pages of btrfs.readthedocs.io.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_FORMAT_H
#define NXU_VFS_BTRFS_BTRFS_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- status codes -------------------------------------------------------- */

typedef enum {
	BTRFS_OK = 0,
	BTRFS_ERR_INVALID,             /* bad argument */
	BTRFS_ERR_NOMEM,
	BTRFS_ERR_IO,                  /* the device failed a read */
	BTRFS_ERR_BAD_MAGIC,           /* no Btrfs superblock */
	BTRFS_ERR_CSUM,                /* a checksum did not match */
	BTRFS_ERR_CORRUPT,             /* structurally invalid metadata */
	BTRFS_ERR_TRUNCATED,           /* the device is smaller than the filesystem */
	BTRFS_ERR_NOT_FOUND,
	BTRFS_ERR_END,                 /* iteration finished (not an error) */
	BTRFS_ERR_NOT_DIRECTORY,
	BTRFS_ERR_IS_DIRECTORY,
	BTRFS_ERR_NAME_TOO_LONG,
	BTRFS_ERR_UNSUPPORTED,         /* generic: valid but not implemented */
	BTRFS_ERR_UNSUPPORTED_FEATURE, /* unknown or unsupported incompat/compat_ro bits */
	BTRFS_ERR_UNSUPPORTED_CSUM,    /* checksum algorithm this driver does not know (none today) */
	BTRFS_ERR_UNSUPPORTED_PROFILE, /* RAID / multi-device chunk or filesystem */
	BTRFS_ERR_UNSUPPORTED_COMPRESSION,
	BTRFS_ERR_UNSUPPORTED_ENCRYPTION,
	BTRFS_ERR_LOG_TREE,            /* the log tree holds something the replay layer does not understand */
	BTRFS_ERR_MISSING_DEVICE       /* a device of the filesystem was not supplied (degraded mounts are refused) */
} btrfs_status_t;

const char *btrfs_status_name(btrfs_status_t status);

/* ---- little-endian accessors --------------------------------------------- */

static inline uint16_t btrfs_get_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static inline uint32_t btrfs_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static inline uint64_t btrfs_get_le64(const uint8_t *p)
{
	return (uint64_t)btrfs_get_le32(p) | ((uint64_t)btrfs_get_le32(p + 4) << 32U);
}

static inline void btrfs_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8U);
	p[2] = (uint8_t)(v >> 16U);
	p[3] = (uint8_t)(v >> 24U);
}

static inline void btrfs_put_le64(uint8_t *p, uint64_t v)
{
	btrfs_put_le32(p, (uint32_t)v);
	btrfs_put_le32(p + 4, (uint32_t)(v >> 32U));
}

/* ---- sizes and limits ---------------------------------------------------- */

#define BTRFS_MAGIC 0x4D5F53665248425FULL /* "_BHRfS_M" */
#define BTRFS_SUPER_INFO_OFFSET 0x10000ULL
#define BTRFS_SUPER_INFO_SIZE 4096U
#define BTRFS_SUPER_MIRROR_MAX 3U
#define BTRFS_SUPER_MIRROR_SHIFT 12U

#define BTRFS_CSUM_SIZE 32U
#define BTRFS_FSID_SIZE 16U
#define BTRFS_UUID_SIZE 16U
#define BTRFS_LABEL_SIZE 256U
#define BTRFS_SYSTEM_CHUNK_ARRAY_SIZE 2048U
#define BTRFS_MAX_LEVEL 8U
#define BTRFS_NAME_LEN 255U
#define BTRFS_MAX_METADATA_BLOCKSIZE 65536U
#define BTRFS_MIN_SECTORSIZE 512U
#define BTRFS_MAX_SECTORSIZE 65536U
#define BTRFS_CHUNK_MAX_STRIPES 8U

/* ---- objectids ----------------------------------------------------------- */

#define BTRFS_ROOT_TREE_OBJECTID 1ULL
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL
#define BTRFS_CHUNK_TREE_OBJECTID 3ULL
#define BTRFS_DEV_TREE_OBJECTID 4ULL
#define BTRFS_FS_TREE_OBJECTID 5ULL
#define BTRFS_ROOT_TREE_DIR_OBJECTID 6ULL
#define BTRFS_CSUM_TREE_OBJECTID 7ULL
#define BTRFS_QUOTA_TREE_OBJECTID 8ULL
#define BTRFS_UUID_TREE_OBJECTID 9ULL
#define BTRFS_FREE_SPACE_TREE_OBJECTID 10ULL
#define BTRFS_BLOCK_GROUP_TREE_OBJECTID 11ULL
#define BTRFS_RAID_STRIPE_TREE_OBJECTID 12ULL
#define BTRFS_DEV_ITEMS_OBJECTID 1ULL
#define BTRFS_EXTENT_CSUM_OBJECTID ((uint64_t)-10LL)
#define BTRFS_TREE_LOG_OBJECTID ((uint64_t)-6LL)
#define BTRFS_FIRST_FREE_OBJECTID 256ULL
#define BTRFS_LAST_FREE_OBJECTID ((uint64_t)-256LL)
#define BTRFS_FIRST_CHUNK_TREE_OBJECTID 256ULL

/* ---- key types ----------------------------------------------------------- */

#define BTRFS_INODE_ITEM_KEY 1U
#define BTRFS_INODE_REF_KEY 12U
#define BTRFS_INODE_EXTREF_KEY 13U
#define BTRFS_XATTR_ITEM_KEY 24U
#define BTRFS_ORPHAN_ITEM_KEY 48U
#define BTRFS_DIR_LOG_ITEM_KEY 60U
#define BTRFS_DIR_LOG_INDEX_KEY 72U
#define BTRFS_DIR_ITEM_KEY 84U
#define BTRFS_DIR_INDEX_KEY 96U
#define BTRFS_EXTENT_DATA_KEY 108U
#define BTRFS_EXTENT_CSUM_KEY 128U
#define BTRFS_ROOT_ITEM_KEY 132U
#define BTRFS_ROOT_BACKREF_KEY 144U
#define BTRFS_ROOT_REF_KEY 156U
#define BTRFS_EXTENT_ITEM_KEY 168U
#define BTRFS_METADATA_ITEM_KEY 169U
#define BTRFS_BLOCK_GROUP_ITEM_KEY 192U
#define BTRFS_DEV_EXTENT_KEY 204U
#define BTRFS_DEV_ITEM_KEY 216U
#define BTRFS_CHUNK_ITEM_KEY 228U
#define BTRFS_RAID_STRIPE_KEY 230U
#define BTRFS_UUID_KEY_SUBVOL 251U
#define BTRFS_UUID_KEY_RECEIVED_SUBVOL 252U

/* ---- file types in directory items ---------------------------------------- */

#define BTRFS_FT_UNKNOWN 0U
#define BTRFS_FT_REG_FILE 1U
#define BTRFS_FT_DIR 2U
#define BTRFS_FT_CHRDEV 3U
#define BTRFS_FT_BLKDEV 4U
#define BTRFS_FT_FIFO 5U
#define BTRFS_FT_SOCK 6U
#define BTRFS_FT_SYMLINK 7U
#define BTRFS_FT_XATTR 8U

/* ---- POSIX mode bits ------------------------------------------------------ */

#define BTRFS_S_IFMT 0170000U
#define BTRFS_S_IFSOCK 0140000U
#define BTRFS_S_IFLNK 0120000U
#define BTRFS_S_IFREG 0100000U
#define BTRFS_S_IFBLK 0060000U
#define BTRFS_S_IFDIR 0040000U
#define BTRFS_S_IFCHR 0020000U
#define BTRFS_S_IFIFO 0010000U

/* ---- feature flags -------------------------------------------------------- */

#define BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF (1ULL << 0U)
#define BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL (1ULL << 1U)
#define BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS (1ULL << 2U)
#define BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO (1ULL << 3U)
#define BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD (1ULL << 4U)
#define BTRFS_FEATURE_INCOMPAT_BIG_METADATA (1ULL << 5U)
#define BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF (1ULL << 6U)
#define BTRFS_FEATURE_INCOMPAT_RAID56 (1ULL << 7U)
#define BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA (1ULL << 8U)
#define BTRFS_FEATURE_INCOMPAT_NO_HOLES (1ULL << 9U)
#define BTRFS_FEATURE_INCOMPAT_METADATA_UUID (1ULL << 10U)
#define BTRFS_FEATURE_INCOMPAT_RAID1C34 (1ULL << 11U)
#define BTRFS_FEATURE_INCOMPAT_ZONED (1ULL << 12U)
#define BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2 (1ULL << 13U)
#define BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE (1ULL << 14U)
#define BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA (1ULL << 16U)

#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE (1ULL << 0U)
#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID (1ULL << 1U)
#define BTRFS_FEATURE_COMPAT_RO_VERITY (1ULL << 2U)
#define BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE (1ULL << 3U)

/*
 * Incompat bits the read path understands. Everything else is refused: an
 * incompat bit means an old reader would misread the filesystem.
 *
 *   MIXED_BACKREF, BIG_METADATA, EXTENDED_IREF, SKINNY_METADATA, MIXED_GROUPS,
 *   SIMPLE_QUOTA    change only how metadata is written or accounted for, not
 *                   how a reader walks the fs trees.
 *   DEFAULT_SUBVOL  the root tree carries a "default" directory item.
 *   NO_HOLES        holes have no extent item (a gap reads as zeros anyway).
 *   METADATA_UUID   tree blocks carry metadata_uuid, not fsid, in their header.
 *   COMPRESS_LZO/ZSTD  are only *flags*; compressed extents are refused per
 *                   extent, so the rest of the filesystem stays readable.
 *   RAID1C34, RAID56  are accepted here and refused by the chunk layer, which
 *                   knows the actual profile in use.
 * Refused: ZONED, EXTENT_TREE_V2, RAID_STRIPE_TREE and any unknown bit.
 */
#define BTRFS_INCOMPAT_SUPPORTED ( \
	BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF | BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL | \
	BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS | BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO | \
	BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD | BTRFS_FEATURE_INCOMPAT_BIG_METADATA | \
	BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF | BTRFS_FEATURE_INCOMPAT_RAID56 | \
	BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA | BTRFS_FEATURE_INCOMPAT_NO_HOLES | \
	BTRFS_FEATURE_INCOMPAT_METADATA_UUID | BTRFS_FEATURE_INCOMPAT_RAID1C34 | \
	BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA)

/* compat_ro bits a read-only mount may ignore. Unknown ones are ignored too:
 * that is precisely what compat_ro means for a read-only mount. */
#define BTRFS_COMPAT_RO_KNOWN ( \
	BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE | BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID | \
	BTRFS_FEATURE_COMPAT_RO_VERITY | BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE)

#define BTRFS_CSUM_TYPE_CRC32 0U
#define BTRFS_CSUM_TYPE_XXHASH 1U
#define BTRFS_CSUM_TYPE_SHA256 2U
#define BTRFS_CSUM_TYPE_BLAKE2 3U

/* ---- block group / chunk types --------------------------------------------- */

#define BTRFS_BLOCK_GROUP_DATA (1ULL << 0U)
#define BTRFS_BLOCK_GROUP_SYSTEM (1ULL << 1U)
#define BTRFS_BLOCK_GROUP_METADATA (1ULL << 2U)
#define BTRFS_BLOCK_GROUP_RAID0 (1ULL << 3U)
#define BTRFS_BLOCK_GROUP_RAID1 (1ULL << 4U)
#define BTRFS_BLOCK_GROUP_DUP (1ULL << 5U)
#define BTRFS_BLOCK_GROUP_RAID10 (1ULL << 6U)
#define BTRFS_BLOCK_GROUP_RAID5 (1ULL << 7U)
#define BTRFS_BLOCK_GROUP_RAID6 (1ULL << 8U)
#define BTRFS_BLOCK_GROUP_RAID1C3 (1ULL << 9U)
#define BTRFS_BLOCK_GROUP_RAID1C4 (1ULL << 10U)
#define BTRFS_BLOCK_GROUP_TYPE_MASK (BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_METADATA)
#define BTRFS_BLOCK_GROUP_PROFILE_MASK ( \
	BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP | \
	BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6 | \
	BTRFS_BLOCK_GROUP_RAID1C3 | BTRFS_BLOCK_GROUP_RAID1C4)

/* ---- file extents ---------------------------------------------------------- */

#define BTRFS_FILE_EXTENT_INLINE 0U
#define BTRFS_FILE_EXTENT_REG 1U
#define BTRFS_FILE_EXTENT_PREALLOC 2U

#define BTRFS_COMPRESS_NONE 0U
#define BTRFS_COMPRESS_ZLIB 1U
#define BTRFS_COMPRESS_LZO 2U
#define BTRFS_COMPRESS_ZSTD 3U

#define BTRFS_INODE_NODATASUM (1ULL << 0U)
#define BTRFS_INODE_NODATACOW (1ULL << 1U)
#define BTRFS_INODE_READONLY (1ULL << 2U)
#define BTRFS_INODE_NOCOMPRESS (1ULL << 3U)
#define BTRFS_INODE_PREALLOC (1ULL << 4U)

#define BTRFS_ROOT_SUBVOL_RDONLY (1ULL << 0U)

/* ---- keys ------------------------------------------------------------------ */

typedef struct {
	uint64_t objectid;
	uint8_t type;
	uint64_t offset;
} btrfs_key_t;

#define BTRFS_DISK_KEY_SIZE 17U

static inline void btrfs_key_read(const uint8_t *p, btrfs_key_t *key)
{
	key->objectid = btrfs_get_le64(p);
	key->type = p[8];
	key->offset = btrfs_get_le64(p + 9);
}

/* Order by objectid, then type, then offset: the order every tree is sorted in. */
static inline int btrfs_key_cmp(const btrfs_key_t *a, const btrfs_key_t *b)
{
	if (a->objectid != b->objectid) return a->objectid < b->objectid ? -1 : 1;
	if (a->type != b->type) return a->type < b->type ? -1 : 1;
	if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
	return 0;
}

static inline btrfs_key_t btrfs_key_make(uint64_t objectid, uint8_t type, uint64_t offset)
{
	btrfs_key_t key = { objectid, type, offset };
	return key;
}

/* ---- superblock ------------------------------------------------------------- */

#define BTRFS_SB_CSUM 0U
#define BTRFS_SB_FSID 32U
#define BTRFS_SB_BYTENR 48U
#define BTRFS_SB_FLAGS 56U
#define BTRFS_SB_MAGIC 64U
#define BTRFS_SB_GENERATION 72U
#define BTRFS_SB_ROOT 80U
#define BTRFS_SB_CHUNK_ROOT 88U
#define BTRFS_SB_LOG_ROOT 96U
#define BTRFS_SB_TOTAL_BYTES 112U
#define BTRFS_SB_BYTES_USED 120U
#define BTRFS_SB_ROOT_DIR_OBJECTID 128U
#define BTRFS_SB_NUM_DEVICES 136U
#define BTRFS_SB_SECTORSIZE 144U
#define BTRFS_SB_NODESIZE 148U
#define BTRFS_SB_STRIPESIZE 156U
#define BTRFS_SB_SYS_CHUNK_ARRAY_SIZE 160U
#define BTRFS_SB_CHUNK_ROOT_GENERATION 164U
#define BTRFS_SB_COMPAT_FLAGS 172U
#define BTRFS_SB_COMPAT_RO_FLAGS 180U
#define BTRFS_SB_INCOMPAT_FLAGS 188U
#define BTRFS_SB_CSUM_TYPE 196U
#define BTRFS_SB_ROOT_LEVEL 198U
#define BTRFS_SB_CHUNK_ROOT_LEVEL 199U
#define BTRFS_SB_LOG_ROOT_LEVEL 200U
#define BTRFS_SB_DEV_ITEM 201U
#define BTRFS_SB_LABEL 299U
#define BTRFS_SB_CACHE_GENERATION 555U
#define BTRFS_SB_UUID_TREE_GENERATION 563U
#define BTRFS_SB_METADATA_UUID 571U
#define BTRFS_SB_SYS_CHUNK_ARRAY 811U

/* Offsets inside the embedded btrfs_dev_item. */
#define BTRFS_DEV_ITEM_SIZE 98U
#define BTRFS_DEV_DEVID 0U
#define BTRFS_DEV_TOTAL_BYTES 8U
#define BTRFS_DEV_BYTES_USED 16U
#define BTRFS_DEV_UUID 66U

typedef struct {
	uint8_t csum[BTRFS_CSUM_SIZE];
	uint8_t fsid[BTRFS_FSID_SIZE];
	uint8_t metadata_uuid[BTRFS_FSID_SIZE];
	uint64_t bytenr;          /* where this copy claims to live */
	uint64_t flags;
	uint64_t generation;
	uint64_t root;            /* root tree */
	uint64_t chunk_root;
	uint64_t log_root;
	uint64_t total_bytes;
	uint64_t bytes_used;
	uint64_t root_dir_objectid;
	uint64_t num_devices;
	uint32_t sectorsize;
	uint32_t nodesize;
	uint32_t stripesize;
	uint32_t sys_chunk_array_size;
	uint64_t chunk_root_generation;
	uint64_t compat_flags;
	uint64_t compat_ro_flags;
	uint64_t incompat_flags;
	uint16_t csum_type;
	uint8_t root_level;
	uint8_t chunk_root_level;
	uint8_t log_root_level;
	uint64_t dev_id;
	uint64_t dev_total_bytes;
	uint64_t dev_bytes_used;
	uint8_t dev_uuid[BTRFS_UUID_SIZE];
	uint64_t cache_generation;
	uint64_t uuid_tree_generation;
	char label[BTRFS_LABEL_SIZE + 1U];
	uint8_t sys_chunk_array[BTRFS_SYSTEM_CHUNK_ARRAY_SIZE];
} btrfs_super_t;

/* ---- tree block header ------------------------------------------------------- */

#define BTRFS_HEADER_SIZE 101U
#define BTRFS_HDR_CSUM 0U
#define BTRFS_HDR_FSID 32U
#define BTRFS_HDR_BYTENR 48U
#define BTRFS_HDR_FLAGS 56U
#define BTRFS_HDR_CHUNK_TREE_UUID 64U
#define BTRFS_HDR_GENERATION 80U
#define BTRFS_HDR_OWNER 88U
#define BTRFS_HDR_NRITEMS 96U
#define BTRFS_HDR_LEVEL 100U

#define BTRFS_ITEM_SIZE 25U      /* struct btrfs_item: key, offset, size */
#define BTRFS_KEY_PTR_SIZE 33U   /* struct btrfs_key_ptr: key, blockptr, generation */

/* ---- chunk items --------------------------------------------------------------- */

#define BTRFS_CHUNK_SIZE 48U         /* without the stripes */
#define BTRFS_STRIPE_SIZE 32U

typedef struct {
	uint64_t devid;
	uint64_t offset;           /* physical byte offset on that device */
} btrfs_stripe_t;

typedef struct {
	uint64_t logical;          /* key offset */
	uint64_t length;
	uint64_t owner;
	uint64_t stripe_len;
	uint64_t type;             /* BTRFS_BLOCK_GROUP_* */
	uint32_t io_align;
	uint32_t io_width;
	uint32_t sector_size;
	uint16_t num_stripes;
	uint16_t sub_stripes;
	btrfs_stripe_t stripes[BTRFS_CHUNK_MAX_STRIPES];
} btrfs_chunk_t;

/*
 * Parse a chunk item. On success *used is the number of bytes the item took
 * (BTRFS_CHUNK_SIZE + num_stripes * BTRFS_STRIPE_SIZE).
 */
static inline bool btrfs_chunk_parse(const uint8_t *p, size_t avail, uint64_t logical, btrfs_chunk_t *chunk, size_t *used)
{
	if (avail < BTRFS_CHUNK_SIZE) return false;

	chunk->logical = logical;
	chunk->length = btrfs_get_le64(p);
	chunk->owner = btrfs_get_le64(p + 8);
	chunk->stripe_len = btrfs_get_le64(p + 16);
	chunk->type = btrfs_get_le64(p + 24);
	chunk->io_align = btrfs_get_le32(p + 32);
	chunk->io_width = btrfs_get_le32(p + 36);
	chunk->sector_size = btrfs_get_le32(p + 40);
	chunk->num_stripes = btrfs_get_le16(p + 44);
	chunk->sub_stripes = btrfs_get_le16(p + 46);

	if (chunk->num_stripes == 0U || chunk->num_stripes > BTRFS_CHUNK_MAX_STRIPES) return false;
	if (avail - BTRFS_CHUNK_SIZE < (size_t)chunk->num_stripes * BTRFS_STRIPE_SIZE) return false;

	for (uint32_t index = 0U; index < BTRFS_CHUNK_MAX_STRIPES; index++) {
		chunk->stripes[index].devid = 0ULL;
		chunk->stripes[index].offset = 0ULL;
	}

	for (uint32_t index = 0U; index < chunk->num_stripes; index++) {
		const uint8_t *s = p + BTRFS_CHUNK_SIZE + (size_t)index * BTRFS_STRIPE_SIZE;
		chunk->stripes[index].devid = btrfs_get_le64(s);
		chunk->stripes[index].offset = btrfs_get_le64(s + 8);
	}

	*used = BTRFS_CHUNK_SIZE + (size_t)chunk->num_stripes * BTRFS_STRIPE_SIZE;
	return true;
}

/* ---- inode item ------------------------------------------------------------------ */

#define BTRFS_INODE_ITEM_SIZE 160U

typedef struct {
	uint64_t sec;
	uint32_t nsec;
} btrfs_timespec_t;

typedef struct {
	uint64_t generation;
	uint64_t transid;
	uint64_t size;
	uint64_t nbytes;
	uint64_t block_group;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t mode;
	uint64_t rdev;
	uint64_t flags;
	uint64_t sequence;
	btrfs_timespec_t atime;
	btrfs_timespec_t ctime;
	btrfs_timespec_t mtime;
	btrfs_timespec_t otime;
} btrfs_inode_item_t;

static inline void btrfs_timespec_read(const uint8_t *p, btrfs_timespec_t *t)
{
	t->sec = btrfs_get_le64(p);
	t->nsec = btrfs_get_le32(p + 8);
}

static inline bool btrfs_inode_item_parse(const uint8_t *p, size_t avail, btrfs_inode_item_t *inode)
{
	if (avail < BTRFS_INODE_ITEM_SIZE) return false;

	inode->generation = btrfs_get_le64(p + 0);
	inode->transid = btrfs_get_le64(p + 8);
	inode->size = btrfs_get_le64(p + 16);
	inode->nbytes = btrfs_get_le64(p + 24);
	inode->block_group = btrfs_get_le64(p + 32);
	inode->nlink = btrfs_get_le32(p + 40);
	inode->uid = btrfs_get_le32(p + 44);
	inode->gid = btrfs_get_le32(p + 48);
	inode->mode = btrfs_get_le32(p + 52);
	inode->rdev = btrfs_get_le64(p + 56);
	inode->flags = btrfs_get_le64(p + 64);
	inode->sequence = btrfs_get_le64(p + 72);
	btrfs_timespec_read(p + 112, &inode->atime);
	btrfs_timespec_read(p + 124, &inode->ctime);
	btrfs_timespec_read(p + 136, &inode->mtime);
	btrfs_timespec_read(p + 148, &inode->otime);
	return true;
}

/* ---- directory items --------------------------------------------------------------- */

#define BTRFS_DIR_ITEM_HEADER_SIZE 30U

typedef struct {
	btrfs_key_t location;      /* INODE_ITEM key, or ROOT_ITEM key for a subvolume point */
	uint64_t transid;
	uint16_t data_len;
	uint16_t name_len;
	uint8_t type;              /* BTRFS_FT_* */
	const uint8_t *name;       /* points into the item */
	const uint8_t *data;       /* xattr value, points into the item */
} btrfs_dir_item_t;

/*
 * Parse one entry of a DIR_ITEM/DIR_INDEX/XATTR_ITEM item. *used is the
 * total size of the entry; the next packed entry (hash collisions pack several
 * per item) starts there.
 */
static inline bool btrfs_dir_item_parse(const uint8_t *p, size_t avail, btrfs_dir_item_t *entry, size_t *used)
{
	if (avail < BTRFS_DIR_ITEM_HEADER_SIZE) return false;

	btrfs_key_read(p, &entry->location);
	entry->transid = btrfs_get_le64(p + 17);
	entry->data_len = btrfs_get_le16(p + 25);
	entry->name_len = btrfs_get_le16(p + 27);
	entry->type = p[29];

	size_t body = (size_t)entry->name_len + (size_t)entry->data_len;
	if (entry->name_len == 0U || entry->name_len > BTRFS_NAME_LEN) return false;
	if (avail - BTRFS_DIR_ITEM_HEADER_SIZE < body) return false;

	entry->name = p + BTRFS_DIR_ITEM_HEADER_SIZE;
	entry->data = p + BTRFS_DIR_ITEM_HEADER_SIZE + entry->name_len;
	*used = BTRFS_DIR_ITEM_HEADER_SIZE + body;
	return true;
}

/* ---- file extent items ---------------------------------------------------------------- */

#define BTRFS_FILE_EXTENT_INLINE_HEADER 21U   /* bytes before the inline data */
#define BTRFS_FILE_EXTENT_SIZE 53U            /* a regular / prealloc item */

typedef struct {
	uint64_t generation;
	uint64_t ram_bytes;        /* decoded size of the whole extent */
	uint8_t compression;
	uint8_t encryption;
	uint16_t other_encoding;
	uint8_t type;
	uint64_t disk_bytenr;      /* 0: a hole */
	uint64_t disk_num_bytes;
	uint64_t offset;           /* start inside the decoded extent */
	uint64_t num_bytes;        /* bytes of the file this item covers */
	uint32_t inline_size;      /* inline: bytes of data following the header */
} btrfs_file_extent_t;

/* item_size is the size of the whole item; inline data length derives from it. */
static inline bool btrfs_file_extent_parse(const uint8_t *p, size_t item_size, btrfs_file_extent_t *e)
{
	if (item_size < BTRFS_FILE_EXTENT_INLINE_HEADER) return false;

	e->generation = btrfs_get_le64(p);
	e->ram_bytes = btrfs_get_le64(p + 8);
	e->compression = p[16];
	e->encryption = p[17];
	e->other_encoding = btrfs_get_le16(p + 18);
	e->type = p[20];
	e->disk_bytenr = 0ULL;
	e->disk_num_bytes = 0ULL;
	e->offset = 0ULL;
	e->num_bytes = 0ULL;
	e->inline_size = 0U;

	if (e->type == BTRFS_FILE_EXTENT_INLINE) {
		e->inline_size = (uint32_t)(item_size - BTRFS_FILE_EXTENT_INLINE_HEADER);
		return true;
	}

	if (e->type != BTRFS_FILE_EXTENT_REG && e->type != BTRFS_FILE_EXTENT_PREALLOC) return false;
	if (item_size < BTRFS_FILE_EXTENT_SIZE) return false;

	e->disk_bytenr = btrfs_get_le64(p + 21);
	e->disk_num_bytes = btrfs_get_le64(p + 29);
	e->offset = btrfs_get_le64(p + 37);
	e->num_bytes = btrfs_get_le64(p + 45);
	return true;
}

/* ---- root items -------------------------------------------------------------------------- */

#define BTRFS_ROOT_ITEM_LEGACY_SIZE 239U
#define BTRFS_ROOT_ITEM_SIZE 439U

typedef struct {
	uint64_t generation;
	uint64_t root_dirid;
	uint64_t bytenr;           /* root node of the tree */
	uint64_t bytes_used;
	uint64_t last_snapshot;
	uint64_t flags;
	uint32_t refs;
	uint8_t level;
	uint64_t generation_v2;    /* 0 for a legacy item */
	uint8_t uuid[BTRFS_UUID_SIZE];
	uint8_t parent_uuid[BTRFS_UUID_SIZE];
	uint8_t received_uuid[BTRFS_UUID_SIZE];
	uint64_t ctransid;
	uint64_t otransid;
	btrfs_timespec_t otime;
} btrfs_root_item_t;

static inline bool btrfs_root_item_parse(const uint8_t *p, size_t avail, btrfs_root_item_t *root)
{
	if (avail < BTRFS_ROOT_ITEM_LEGACY_SIZE) return false;

	root->generation = btrfs_get_le64(p + 160);
	root->root_dirid = btrfs_get_le64(p + 168);
	root->bytenr = btrfs_get_le64(p + 176);
	root->bytes_used = btrfs_get_le64(p + 192);
	root->last_snapshot = btrfs_get_le64(p + 200);
	root->flags = btrfs_get_le64(p + 208);
	root->refs = btrfs_get_le32(p + 216);
	root->level = p[238];
	root->generation_v2 = 0ULL;
	root->ctransid = 0ULL;
	root->otransid = 0ULL;
	root->otime.sec = 0ULL;
	root->otime.nsec = 0U;
	for (uint32_t i = 0U; i < BTRFS_UUID_SIZE; i++) {
		root->uuid[i] = 0U;
		root->parent_uuid[i] = 0U;
		root->received_uuid[i] = 0U;
	}

	if (avail >= BTRFS_ROOT_ITEM_SIZE) {
		root->generation_v2 = btrfs_get_le64(p + 239);
		for (uint32_t i = 0U; i < BTRFS_UUID_SIZE; i++) {
			root->uuid[i] = p[247 + i];
			root->parent_uuid[i] = p[263 + i];
			root->received_uuid[i] = p[279 + i];
		}
		root->ctransid = btrfs_get_le64(p + 295);
		root->otransid = btrfs_get_le64(p + 303);
		btrfs_timespec_read(p + 339, &root->otime);
	}

	return true;
}

/* ---- root refs -------------------------------------------------------------------------------- */

#define BTRFS_ROOT_REF_HEADER_SIZE 18U

typedef struct {
	uint64_t dirid;            /* directory in the parent subvolume holding the point */
	uint64_t sequence;         /* the DIR_INDEX of the subvolume point */
	uint16_t name_len;
	const uint8_t *name;
} btrfs_root_ref_t;

static inline bool btrfs_root_ref_parse(const uint8_t *p, size_t avail, btrfs_root_ref_t *ref)
{
	if (avail < BTRFS_ROOT_REF_HEADER_SIZE) return false;

	ref->dirid = btrfs_get_le64(p);
	ref->sequence = btrfs_get_le64(p + 8);
	ref->name_len = btrfs_get_le16(p + 16);
	if (ref->name_len == 0U || ref->name_len > BTRFS_NAME_LEN) return false;
	if (avail - BTRFS_ROOT_REF_HEADER_SIZE < ref->name_len) return false;
	ref->name = p + BTRFS_ROOT_REF_HEADER_SIZE;
	return true;
}

/* ---- checksums and hashes ------------------------------------------------------------------------ */

/* Running CRC-32C, no final complement (the Linux crc32c() convention). */
uint32_t btrfs_crc32c(uint32_t crc, const void *data, size_t length);

/* The checksum Btrfs stores for a block: ~crc32c(~0, data), little-endian. */
uint32_t btrfs_csum_crc32c(const void *data, size_t length);

/*
 * The checksum of `type` over data, into out[0..csum_size) with the rest of
 * the 32-byte field zeroed (the on-disk form). False for a type this driver
 * does not know.
 */
bool btrfs_csum_data(uint32_t type, const void *data, size_t length, uint8_t out[BTRFS_CSUM_SIZE]);

/*
 * True if the checksum of `data` under `type` equals the csum_size(type) bytes
 * at stored (the first bytes of a header's csum field, or one csum item entry).
 * False for a mismatch and for a type this driver does not know.
 */
bool btrfs_csum_matches(uint32_t type, const uint8_t *stored, const void *data, size_t length);

/* SHA-256 and BLAKE2b with a 32-byte digest (btrfs_hash.c): the 32-byte checksum types. */
void btrfs_sha256(const void *data, size_t length, uint8_t out[32]);
void btrfs_blake2b_256(const void *data, size_t length, uint8_t out[32]);

/* XXH64 (Yann Collet's xxHash, 64-bit) of data with the given seed: Btrfs' xxhash checksum and the ZSTD content checksum. */
uint64_t btrfs_xxh64(const void *data, size_t length, uint64_t seed);

/* The DIR_ITEM key offset for a name: crc32c(~1, name). */
uint32_t btrfs_name_hash(const uint8_t *name, size_t length);

/* The INODE_EXTREF key offset for (parent, name). */
uint64_t btrfs_extref_hash(uint64_t parent_objectid, const uint8_t *name, size_t length);

/* Number of checksum bytes for a csum type, or 0 for an unknown type. */
uint32_t btrfs_csum_type_size(uint32_t type);
const char *btrfs_csum_type_name(uint32_t type);
const char *btrfs_compression_name(uint32_t compression);

#endif
