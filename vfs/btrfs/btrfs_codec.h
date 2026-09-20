/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_codec.h
 *
 * Decoders for compressed file extents (zlib, LZO, ZSTD). Freestanding: no
 * kernel headers, no libc beyond string.h, allocation only through the
 * injected btrfs_env_t. They all have the btrfs_decompress_fn signature of
 * btrfs_fs.h and are plugged into fs->decompressors[] by btrfs_fs_open().
 *
 * Contract shared by all three:
 *
 *   - in/in_len is the on-disk extent (disk_num_bytes; may end in zero
 *     padding up to the sector size), out/out_len is the decoded extent
 *     (ram_bytes). The decoder writes at most out_len bytes, ever, and
 *     returns BTRFS_OK only for a complete, well-formed stream. That is the
 *     decompression-bomb limit: the caller chose out_len (at most
 *     BTRFS_MAX_COMPRESSED_EXTENT), the stream cannot make it larger. A
 *     complete stream that decodes to fewer bytes is zero-filled to out_len
 *     (Linux does the same: the last extent of a file is rounded up to a
 *     sector but only the file's bytes were compressed).
 *   - Anything else (truncated or malformed input, a stream that would decode
 *     to more bytes, a reference before the start of the output, an
 *     unsupported feature) is BTRFS_ERR_CORRUPT, or BTRFS_ERR_NOMEM if the
 *     work memory could not be had. Nothing is read outside in[0..in_len).
 *   - out may hold partial garbage after a failure; the caller discards it.
 *
 * ctx is a btrfs_codec_ctx_t (btrfs_fs.h).
 */

#ifndef NXU_VFS_BTRFS_BTRFS_CODEC_H
#define NXU_VFS_BTRFS_BTRFS_CODEC_H

#include "btrfs_fs.h"

/* Btrfs never writes a compressed extent that decodes to more than this. */
#define BTRFS_MAX_COMPRESSED_EXTENT (128U * 1024U)

btrfs_status_t btrfs_zlib_decompress(void *ctx, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
btrfs_status_t btrfs_lzo_decompress(void *ctx, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

/* One LZO1X stream (no btrfs framing) into out[0..out_cap): the number of bytes produced in *produced. */
btrfs_status_t btrfs_lzo1x_decompress_safe(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *produced);

#endif
