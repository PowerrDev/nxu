/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_fs.c
 *
 * Opening and closing a filesystem, the allocator and log wrappers.
 *
 * Open sequence (each step's failure is a clean, distinct status):
 *   1. superblock: pick the valid copy with the highest generation
 *   2. support check: checksum type, incompat features, device count
 *   3. chunk map bootstrapped from sys_chunk_array, then the full chunk tree
 *   4. the root tree, refusing an unreplayed log tree
 *   5. the subvolume to mount (option, else the "default" directory item)
 */

#include "btrfs_codec.h"
#include "btrfs_fs.h"
#include "btrfs_root.h"
#include "btrfs_tree.h"

#include <string.h>

/* ---- allocator ---------------------------------------------------------------- */

void *btrfs_alloc(const btrfs_fs_t *fs, size_t size)
{
	if (fs == 0 || fs->env.alloc == 0 || size == 0U) return 0;
	return fs->env.alloc(fs->env.ctx, size);
}

void btrfs_free(const btrfs_fs_t *fs, void *pointer)
{
	if (fs == 0 || fs->env.release == 0 || pointer == 0) return;
	fs->env.release(fs->env.ctx, pointer);
}

/* ---- log ------------------------------------------------------------------------ */

#define BTRFS_LOG_LINE_MAX 256U

typedef struct {
	char *out;
	size_t capacity;
	size_t length;
} btrfs_sink_t;

static void btrfs_sink_char(btrfs_sink_t *sink, char c)
{
	if (sink->length + 1U < sink->capacity) sink->out[sink->length] = c;
	sink->length++;
}

static void btrfs_sink_string(btrfs_sink_t *sink, const char *s)
{
	if (s == 0) s = "(null)";
	while (*s != '\0') btrfs_sink_char(sink, *s++);
}

static void btrfs_sink_number(btrfs_sink_t *sink, unsigned long long value, unsigned base)
{
	char digits[24];
	unsigned count = 0U;

	if (value == 0ULL) digits[count++] = '0';

	while (value != 0ULL) {
		unsigned digit = (unsigned)(value % base);
		digits[count++] = (char)(digit < 10U ? '0' + digit : 'a' + digit - 10U);
		value /= base;
	}

	while (count != 0U) btrfs_sink_char(sink, digits[--count]);
}

static void btrfs_vformat(btrfs_sink_t *sink, const char *format, va_list args)
{
	while (*format != '\0') {
		if (*format != '%') {
			btrfs_sink_char(sink, *format++);
			continue;
		}

		format++;
		unsigned length = 0U;
		while (*format == 'l' && length < 2U) {
			length++;
			format++;
		}

		char conversion = *format;
		if (conversion == '\0') break;
		format++;

		switch (conversion) {
		case '%': btrfs_sink_char(sink, '%'); break;
		case 'c': btrfs_sink_char(sink, (char)va_arg(args, int)); break;
		case 's': btrfs_sink_string(sink, va_arg(args, const char *)); break;
		case 'd': {
			long long value;
			if (length == 0U) value = va_arg(args, int);
			else if (length == 1U) value = va_arg(args, long);
			else value = va_arg(args, long long);
			if (value < 0) {
				btrfs_sink_char(sink, '-');
				btrfs_sink_number(sink, (unsigned long long)(-(value + 1)) + 1ULL, 10U);
			} else {
				btrfs_sink_number(sink, (unsigned long long)value, 10U);
			}
			break;
		}
		case 'u':
		case 'x': {
			unsigned long long value;
			if (length == 0U) value = va_arg(args, unsigned int);
			else if (length == 1U) value = va_arg(args, unsigned long);
			else value = va_arg(args, unsigned long long);
			btrfs_sink_number(sink, value, conversion == 'u' ? 10U : 16U);
			break;
		}
		default:
			btrfs_sink_char(sink, '%');
			btrfs_sink_char(sink, conversion);
			break;
		}
	}
}

void btrfs_log(const btrfs_fs_t *fs, const char *format, ...)
{
	if (fs == 0 || fs->env.log == 0) return;

	char line[BTRFS_LOG_LINE_MAX];
	btrfs_sink_t sink = { line, sizeof(line), 0U };
	va_list args;

	va_start(args, format);
	btrfs_vformat(&sink, format, args);
	va_end(args);

	line[sink.length < sizeof(line) ? sink.length : sizeof(line) - 1U] = '\0';
	fs->env.log(fs->env.ctx, line);
}

/* ---- open / close ------------------------------------------------------------------ */

static btrfs_status_t btrfs_open_fail(btrfs_fs_t *fs, btrfs_status_t status, btrfs_status_t *out)
{
	fs->last_status = status;
	*out = status;
	return status;
}

btrfs_fs_t *btrfs_fs_open(const btrfs_env_t *env, const btrfs_reader_t *reader, const btrfs_open_options_t *options, btrfs_status_t *status)
{
	btrfs_status_t local;
	if (status == 0) status = &local;
	*status = BTRFS_ERR_INVALID;

	if (env == 0 || env->alloc == 0 || env->release == 0 || reader == 0 || reader->read == 0) return 0;

	btrfs_fs_t *fs = env->alloc(env->ctx, sizeof(*fs));
	if (fs == 0) {
		*status = BTRFS_ERR_NOMEM;
		return 0;
	}

	fs->env = *env;
	fs->reader = *reader;
	if (options != 0) fs->options = *options;

	btrfs_status_t result = btrfs_super_load(fs, &fs->reader, &fs->super, &fs->super_offset, &fs->super_copies_valid);
	if (result != BTRFS_OK) {
		BTRFS_LOG(fs, "no usable superblock: %s", btrfs_status_name(result));
		btrfs_open_fail(fs, result, status);
		goto fail;
	}

	result = btrfs_super_check_support(fs, &fs->super);
	if (result != BTRFS_OK) {
		btrfs_open_fail(fs, result, status);
		goto fail;
	}

	fs->sectorsize = fs->super.sectorsize;
	fs->nodesize = fs->super.nodesize;
	fs->csum_type = fs->super.csum_type;
	fs->csum_size = btrfs_csum_type_size(fs->super.csum_type);

	fs->codec.env = &fs->env;
	fs->codec.sectorsize = fs->sectorsize;
	fs->decompressors[BTRFS_COMPRESS_ZLIB].decompress = btrfs_zlib_decompress;
	fs->decompressors[BTRFS_COMPRESS_ZLIB].ctx = &fs->codec;
	fs->decompressors[BTRFS_COMPRESS_LZO].decompress = btrfs_lzo_decompress;
	fs->decompressors[BTRFS_COMPRESS_LZO].ctx = &fs->codec;
	memcpy(fs->header_fsid, (fs->super.incompat_flags & BTRFS_FEATURE_INCOMPAT_METADATA_UUID) != 0ULL ? fs->super.metadata_uuid : fs->super.fsid, BTRFS_FSID_SIZE);

	if (fs->super.log_root != 0ULL && !fs->options.ignore_log_tree) {
		BTRFS_LOG(fs, "log tree at %llu holds fsynced changes that need replay; refusing", BTRFS_U64(fs->super.log_root));
		btrfs_open_fail(fs, BTRFS_ERR_LOG_TREE, status);
		goto fail;
	}

	btrfs_cache_init(fs);

	result = btrfs_chunks_bootstrap(fs);
	if (result == BTRFS_OK) result = btrfs_chunks_load(fs);
	if (result != BTRFS_OK) {
		BTRFS_LOG(fs, "chunk map failed: %s", btrfs_status_name(result));
		btrfs_open_fail(fs, result, status);
		goto fail;
	}

	fs->root_tree.objectid = BTRFS_ROOT_TREE_OBJECTID;
	fs->root_tree.bytenr = fs->super.root;
	fs->root_tree.generation = fs->super.generation;
	fs->root_tree.level = fs->super.root_level;

	result = btrfs_root_select_default(fs);
	if (result != BTRFS_OK) {
		BTRFS_LOG(fs, "cannot locate the subvolume to mount: %s", btrfs_status_name(result));
		btrfs_open_fail(fs, result, status);
		goto fail;
	}

	*status = BTRFS_OK;
	return fs;

fail:
	btrfs_fs_close(fs);
	return 0;
}

void btrfs_fs_close(btrfs_fs_t *fs)
{
	if (fs == 0) return;

	btrfs_cache_release(fs);
	btrfs_chunks_release(fs);

	btrfs_env_t env = fs->env;
	if (fs->extent_cache.data != 0) env.release(env.ctx, fs->extent_cache.data);
	env.release(env.ctx, fs);
}

void btrfs_fs_describe(const btrfs_fs_t *fs)
{
	if (fs == 0) return;

	char features[128];
	btrfs_feature_names(fs->super.incompat_flags, fs->super.compat_ro_flags, features, sizeof(features));

	BTRFS_LOG(fs, "label \"%s\", generation %llu, %llu of %llu bytes used", fs->super.label, BTRFS_U64(fs->super.generation), BTRFS_U64(fs->super.bytes_used), BTRFS_U64(fs->super.total_bytes));
	BTRFS_LOG(fs, "nodesize %u, sectorsize %u, checksum %s, superblock at %llu (%u valid copies)", fs->nodesize, fs->sectorsize, btrfs_csum_type_name(fs->super.csum_type), BTRFS_U64(fs->super_offset), fs->super_copies_valid);
	BTRFS_LOG(fs, "features: %s", features);
	BTRFS_LOG(fs, "%u chunks, mounted subvolume %llu (default %llu), root tree at %llu level %u", fs->chunks.count, BTRFS_U64(fs->mount_subvol),BTRFS_U64(fs->default_subvol), BTRFS_U64(fs->root_tree.bytenr), (unsigned)fs->root_tree.level);
	BTRFS_LOG(fs, "tree-block cache %u/%u blocks, %llu hits, %llu misses, %llu evictions", fs->cache.count, fs->cache.limit, BTRFS_U64(fs->cache.hits), BTRFS_U64(fs->cache.misses), BTRFS_U64(fs->cache.evictions));
	BTRFS_LOG(fs, "device reads %llu (%llu bytes), checksum failures %llu, mirror fallbacks %llu", BTRFS_U64(fs->stats.device_reads), BTRFS_U64(fs->stats.device_bytes), BTRFS_U64(fs->stats.csum_failures), BTRFS_U64(fs->stats.mirror_fallbacks));
}
