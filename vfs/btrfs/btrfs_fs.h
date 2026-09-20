/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_fs.h
 *
 * The state of one opened filesystem (struct btrfs_fs) and the layered API of
 * the read-only core. The layers, bottom to top:
 *
 *   btrfs_io        read bytes from the device        (btrfs_io.h)
 *   btrfs_super     locate / validate the superblock  (this file, btrfs_super.c)
 *   btrfs_chunk     logical -> physical mapping       (btrfs_chunk.c)
 *   btrfs_tree      tree blocks, cache, search        (btrfs_tree.c)
 *   btrfs_root      roots, subvolumes, default        (btrfs_root.c)
 *   btrfs_inode     inode items, inode refs           (btrfs_inode.c)
 *   btrfs_dir       directory lookup and iteration    (btrfs_dir.c)
 *   btrfs_file      file data, symlinks               (btrfs_file.c)
 *
 * A layer only calls downwards. The core does not lock: the caller (the VFS
 * glue) serialises calls on one btrfs_fs.
 *
 * Write-support extension points (nothing below is implemented; the design
 * notes are in doc/vfs/btrfs.md): every btrfs_tree_t and btrfs_block_t already
 * carries the generation and owner a COW transaction needs, blocks have a
 * dirty flag, and all tree access goes through btrfs_block_get()/the path API
 * so a modifying layer can interpose there.
 */

#ifndef NXU_VFS_BTRFS_BTRFS_FS_H
#define NXU_VFS_BTRFS_BTRFS_FS_H

#include "btrfs_format.h"
#include "btrfs_io.h"

#include <stdarg.h>

struct btrfs_fs;
typedef struct btrfs_fs btrfs_fs_t;

/* ---- log ------------------------------------------------------------------ */

/*
 * Every line starts with the emitting function's name: BTRFS_LOG(fs, "bad %s", x)
 * logs "func: bad x". The formatter is a printf subset (%s %c %d %u %x %llu %llx
 * %lu %lx %%) with no field widths, matching the kernel's kprintf.
 */
void btrfs_log(const btrfs_fs_t *fs, const char *format, ...) __attribute__((format(printf, 2, 3)));

#define BTRFS_LOG(fs, format, ...) btrfs_log((fs), "%s: " format, __func__, ##__VA_ARGS__)
#define BTRFS_U64(value) ((unsigned long long)(value))

/* ---- tree references and blocks -------------------------------------------- */

/*
 * A tree: where its root node is and what to expect there. Subvolumes, the
 * root tree and the chunk tree are all btrfs_tree_t values. A write path would
 * update bytenr/generation/level when a COW transaction commits a new root.
 */
typedef struct {
	uint64_t objectid;       /* tree id (the owner recorded in its blocks) */
	uint64_t bytenr;         /* logical address of the root node */
	uint64_t generation;     /* generation the root must carry (0: unchecked) */
	uint8_t level;
} btrfs_tree_t;

typedef struct btrfs_block btrfs_block_t;

/*
 * A verified, cached tree block. data holds the raw nodesize bytes; only the
 * accessors in btrfs_tree.h interpret them. refs counts holders (paths); a
 * block with refs != 0 is never evicted.
 */
struct btrfs_block {
	uint64_t bytenr;
	uint64_t generation;
	uint64_t owner;
	uint8_t *data;
	uint32_t size;           /* nodesize */
	uint32_t nritems;
	uint32_t refs;
	uint8_t level;
	bool dirty;              /* reserved for the write path */
	btrfs_block_t *hash_next;
	btrfs_block_t *lru_newer;
	btrfs_block_t *lru_older;
};

#define BTRFS_CACHE_BUCKETS 64U

typedef struct {
	btrfs_block_t *buckets[BTRFS_CACHE_BUCKETS];
	btrfs_block_t *newest;
	btrfs_block_t *oldest;
	uint32_t count;
	uint32_t limit;
	uint64_t hits;
	uint64_t misses;
	uint64_t evictions;
} btrfs_cache_t;

/* ---- chunk map ------------------------------------------------------------------ */

typedef struct {
	btrfs_chunk_t *chunks;   /* sorted by logical, non-overlapping */
	uint32_t count;
	uint32_t capacity;
} btrfs_chunk_map_t;

/* Where a logical range lives: up to BTRFS_CHUNK_MAX_STRIPES equivalent copies. */
typedef struct {
	uint32_t copies;         /* SINGLE: 1, DUP: 2 */
	uint64_t physical[BTRFS_CHUNK_MAX_STRIPES];
	uint64_t length;         /* bytes from logical to the end of the chunk */
	uint64_t type;           /* the chunk's BTRFS_BLOCK_GROUP_* flags */
} btrfs_mapping_t;

/* ---- decompression hook -------------------------------------------------------------- */

/*
 * Decoders for compressed extents plug in here (zlib, lzo, zstd). in holds the
 * on-disk (compressed) bytes; out receives exactly out_len decoded bytes and
 * must be fully written. Until one is registered the read path returns
 * BTRFS_ERR_UNSUPPORTED_COMPRESSION for that extent, never garbage.
 */
typedef btrfs_status_t (*btrfs_decompress_fn)(void *ctx, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

typedef struct {
	btrfs_decompress_fn decompress;
	void *ctx;
} btrfs_decompressor_t;

/* ---- open options ------------------------------------------------------------------------ */

typedef struct {
	uint64_t subvol_id;        /* 0: the filesystem's default subvolume */
	bool skip_data_csums;      /* do NOT check data checksums (the default is to check) */
	bool ignore_log_tree;      /* mount despite an unreplayed log tree (may show stale data) */
	uint32_t cache_blocks;     /* tree-block cache size in blocks; 0: automatic */
} btrfs_open_options_t;

typedef struct {
	uint64_t device_reads;     /* reader calls */
	uint64_t device_bytes;
	uint64_t csum_failures;    /* tree blocks or data that failed a checksum */
	uint64_t mirror_fallbacks; /* a second copy served after the first failed */
	uint64_t data_csum_checked;  /* data sectors compared with the csum tree */
	uint64_t data_csum_missing;  /* data sectors of a checksummed file that had no csum item */
	uint64_t data_bad_reads;     /* file reads refused because no copy of some extent verified */
} btrfs_stats_t;

/* ---- the filesystem ------------------------------------------------------------------------ */

struct btrfs_fs {
	btrfs_env_t env;
	btrfs_reader_t reader;
	btrfs_open_options_t options;

	btrfs_super_t super;
	uint64_t super_offset;         /* which mirror was used */
	uint32_t super_copies_valid;
	uint8_t header_fsid[BTRFS_FSID_SIZE];
	uint32_t sectorsize;
	uint32_t nodesize;
	uint32_t csum_type;            /* BTRFS_CSUM_TYPE_*, from the superblock */
	uint32_t csum_size;            /* bytes per checksum: 4, 8 or 32 */

	btrfs_chunk_map_t chunks;
	btrfs_cache_t cache;
	btrfs_tree_t chunk_tree;
	btrfs_tree_t root_tree;
	btrfs_tree_t csum_tree;        /* opened on first use by the data checksum path */
	bool have_csum_tree;
	uint64_t default_subvol;      /* the filesystem's default subvolume id */
	uint64_t mount_subvol;         /* the subvolume this mount serves */

	btrfs_decompressor_t decompressors[4];   /* indexed by BTRFS_COMPRESS_* */
	btrfs_stats_t stats;
	btrfs_status_t last_status;    /* the most recent failure, for diagnostics */
};

/* ---- superblock (btrfs_super.c) --------------------------------------------------------------- */

/*
 * Inspect each superblock copy that fits on the device and return the valid one
 * with the highest generation. *copies_valid counts the valid copies. A copy
 * is valid when its magic, its own byte number, its checksum and its basic
 * geometry are all right.
 */
btrfs_status_t btrfs_super_load(const btrfs_fs_t *fs, const btrfs_reader_t *reader, btrfs_super_t *out, uint64_t *offset_used, uint32_t *copies_valid);

/* Refuse what the driver cannot read: features, checksum type, multiple devices. */
btrfs_status_t btrfs_super_check_support(const btrfs_fs_t *fs, const btrfs_super_t *super);

/* Parse one 4096-byte superblock image; verifies magic/bytenr/checksum/geometry. */
btrfs_status_t btrfs_super_parse(const uint8_t *raw, uint64_t expected_bytenr, uint64_t device_size, btrfs_super_t *out);

/* Names of the feature bits set in flags, for logs ("no-holes,skinny-metadata"). */
void btrfs_feature_names(uint64_t incompat, uint64_t compat_ro, char *out, size_t capacity);

/* ---- chunk map (btrfs_chunk.c) -------------------------------------------------------------------- */

btrfs_status_t btrfs_chunks_bootstrap(btrfs_fs_t *fs);
btrfs_status_t btrfs_chunks_load(btrfs_fs_t *fs);
void btrfs_chunks_release(btrfs_fs_t *fs);
btrfs_status_t btrfs_map_logical(const btrfs_fs_t *fs, uint64_t logical, btrfs_mapping_t *mapping);

/* Read length bytes at a logical address from one copy (0-based). */
btrfs_status_t btrfs_read_logical(btrfs_fs_t *fs, uint64_t logical, void *buffer, size_t length, uint32_t copy);

const char *btrfs_profile_name(uint64_t chunk_type);

/* ---- lifecycle (btrfs_fs.c) --------------------------------------------------------------------------- */

/*
 * Open a filesystem. On failure returns NULL and stores the reason in *status;
 * the failure has already been logged. The returned filesystem is read-only.
 */
btrfs_fs_t *btrfs_fs_open(const btrfs_env_t *env, const btrfs_reader_t *reader, const btrfs_open_options_t *options, btrfs_status_t *status);
void btrfs_fs_close(btrfs_fs_t *fs);

void *btrfs_alloc(const btrfs_fs_t *fs, size_t size);
void btrfs_free(const btrfs_fs_t *fs, void *pointer);

/* One line per fact about the mounted filesystem, for btrfs_dump(). */
void btrfs_fs_describe(const btrfs_fs_t *fs);

#endif
