/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        tools/btrfs/host/btrfs_host.c
 *
 * Host test tool for the Btrfs core (vfs/btrfs/, everything but the VFS glue).
 * The core is compiled natively with the sanitizers and driven over the
 * fixtures under tools/btrfs/fixtures; see tools/btrfs/test_host.sh.
 *
 *   btrfs_host info  IMAGE                       print what the superblock says
 *   btrfs_host open  IMAGE [options]             mount; print the status name
 *   btrfs_host walk  IMAGE [options]             walk the tree with no manifest
 *   btrfs_host check IMAGE MANIFEST [options]    walk the whole tree and compare
 *                                                with the ground-truth manifest
 *   btrfs_host sweep IMAGE --seed N --iters N    corruption sweep
 *
 * options:  --subvol ID  --verify-data  --ignore-log  --seed N  -v
 *
 * The manifest was written inside the Alpine guest by Linux's own Btrfs driver
 * (tools/btrfs/guest/manifest.py), so `check` compares this driver with Linux,
 * not with itself: every path, type, mode, owner, size, link count, inode
 * number, device number, mtime, symlink target, and the crc32c and sha256 of
 * the contents of every file. It also reads each file at random offsets and
 * lengths, across the end of the file, and in odd-sized steps, and compares
 * with the full read; it looks every name up by hash as well as enumerating
 * it; and it resumes readdir from saved cursors.
 *
 * The sweep is the robustness test: deterministic (seeded) corruption of a
 * good image, which must only ever produce errors, or the exact same results
 * as the good image (a DUP mirror can heal a damaged block), never a crash,
 * a hang, an out-of-bounds access, a leak, or different data reported as
 * success.
 */

#include "../../../vfs/btrfs/btrfs_dir.h"
#include "../../../vfs/btrfs/btrfs_file.h"
#include "../../../vfs/btrfs/btrfs_inode.h"
#include "../../../vfs/btrfs/btrfs_io_host.h"
#include "../../../vfs/btrfs/btrfs_root.h"
#include "../../../vfs/btrfs/btrfs_tree.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- sha256 (a second, independent hash of the file contents) ---------------------------- */

typedef struct {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
} sha256_t;

static const uint32_t g_sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = s->state[0]; b = s->state[1]; c = s->state[2]; d = s->state[3];
	e = s->state[4]; f = s->state[5]; g = s->state[6]; h = s->state[7];

	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + g_sha_k[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}

	s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
	s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}

static void sha256_init(sha256_t *s)
{
	static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

	memcpy(s->state, init, sizeof(init));
	s->length = 0;
	s->used = 0;
}

static void sha256_update(sha256_t *s, const uint8_t *data, size_t length)
{
	s->length += length;

	while (length != 0) {
		size_t take = 64 - s->used < length ? 64 - s->used : length;

		memcpy(s->block + s->used, data, take);
		s->used += take;
		data += take;
		length -= take;

		if (s->used == 64) {
			sha256_block(s, s->block);
			s->used = 0;
		}
	}
}

static void sha256_hex(sha256_t *s, char out[65])
{
	uint64_t bits = s->length * 8;
	uint8_t pad = 0x80;

	sha256_update(s, &pad, 1);
	pad = 0;
	while (s->used != 56) sha256_update(s, &pad, 1);

	uint8_t tail[8];
	for (int i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_update(s, tail, 8);

	for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", s->state[i]);
}

/* ---- an image with damage ------------------------------------------------------------------ */

typedef struct {
	uint64_t offset;
	size_t length;
	uint8_t *data;
} patch_t;

typedef struct {
	uint64_t offset;
	size_t length;
	int tag;                 /* 1: data of a NODATASUM file (nothing can protect it) */
} range_t;

#define MAX_PATCHES 16

typedef struct {
	btrfs_host_file_reader_t file;
	btrfs_reader_t base;
	uint64_t size;           /* effective size (truncation) */
	patch_t patches[MAX_PATCHES];
	int patch_count;

	uint64_t reads;
	uint64_t read_budget;    /* 0: unlimited */
	uint64_t fail_read_at;   /* fail the Nth read (1-based), 0: never */
	bool budget_exceeded;

	bool record;
	range_t *ranges;
	size_t range_count;
	size_t range_capacity;
	int tag;
} image_t;

static bool image_read(void *ctx, uint64_t offset, void *buffer, size_t length)
{
	image_t *image = ctx;

	image->reads++;
	if (image->read_budget != 0 && image->reads > image->read_budget) {
		image->budget_exceeded = true;
		return false;
	}
	if (image->fail_read_at != 0 && image->reads == image->fail_read_at) return false;
	if (offset > image->size || length > image->size - offset) return false;

	if (image->record) {
		if (image->range_count == image->range_capacity) {
			image->range_capacity = image->range_capacity ? image->range_capacity * 2 : 1024;
			image->ranges = realloc(image->ranges, image->range_capacity * sizeof(range_t));
			if (image->ranges == NULL) abort();
		}
		image->ranges[image->range_count++] = (range_t){ offset, length, image->tag };
	}

	if (!btrfs_reader_read(&image->base, offset, buffer, length)) return false;

	for (int i = 0; i < image->patch_count; i++) {
		const patch_t *p = &image->patches[i];
		uint64_t start = offset > p->offset ? offset : p->offset;
		uint64_t end = offset + length < p->offset + p->length ? offset + length : p->offset + p->length;

		if (start < end) memcpy((uint8_t *)buffer + (start - offset), p->data + (start - p->offset), (size_t)(end - start));
	}

	return true;
}

static bool image_open(image_t *image, btrfs_reader_t *reader, const char *path)
{
	memset(image, 0, sizeof(*image));

	if (!btrfs_host_file_reader_open(&image->file, &image->base, path)) return false;

	image->size = image->file.size;
	reader->ctx = image;
	reader->size = image->size;
	reader->read = image_read;
	return true;
}

static void image_reset_damage(image_t *image, btrfs_reader_t *reader)
{
	for (int i = 0; i < image->patch_count; i++) free(image->patches[i].data);
	image->patch_count = 0;
	image->size = image->file.size;
	reader->size = image->size;
	image->reads = 0;
	image->read_budget = 0;
	image->fail_read_at = 0;
	image->budget_exceeded = false;
	image->record = false;
}

static void image_patch(image_t *image, uint64_t offset, const uint8_t *bytes, size_t length)
{
	if (image->patch_count == MAX_PATCHES) return;

	patch_t *p = &image->patches[image->patch_count++];
	p->offset = offset;
	p->length = length;
	p->data = malloc(length);
	if (p->data == NULL) abort();
	memcpy(p->data, bytes, length);
}

static void image_close(image_t *image, btrfs_reader_t *reader)
{
	image_reset_damage(image, reader);
	btrfs_host_file_reader_close(&image->file);
	free(image->ranges);
	image->ranges = NULL;
}

/* ---- manifest -------------------------------------------------------------------------------- */

typedef struct {
	char *path;
	char type;
	uint32_t mode, uid, gid, nlink;
	uint64_t size, ino, mtime;
	uint32_t rdev_major, rdev_minor;
	bool has_ino, has_rdev, has_mtime, has_crc, has_size, has_nlink;
	uint32_t crc;
	char sha[65];
	char *target;
	char *flag;
	bool visited;
} record_t;

typedef struct {
	record_t *records;
	size_t count;
} manifest_t;

static int record_compare(const void *a, const void *b)
{
	return strcmp(((const record_t *)a)->path, ((const record_t *)b)->path);
}

static bool manifest_load(manifest_t *m, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) return false;

	size_t capacity = 256;
	m->records = calloc(capacity, sizeof(record_t));
	m->count = 0;

	char *line = NULL;
	size_t line_capacity = 0;
	ssize_t got;

	while ((got = getline(&line, &line_capacity, f)) > 0) {
		if (line[0] == '#') continue;
		if (line[got - 1] == '\n') line[got - 1] = '\0';

		char *fields[14];
		int n = 0;
		char *cursor = line;

		while (n < 14) {
			fields[n++] = cursor;
			char *tab = strchr(cursor, '\t');
			if (tab == NULL) break;
			*tab = '\0';
			cursor = tab + 1;
		}

		if (n != 14) { fprintf(stderr, "manifest %s: bad line\n", path); return false; }

		if (m->count == capacity) {
			capacity *= 2;
			m->records = realloc(m->records, capacity * sizeof(record_t));
			memset(m->records + m->count, 0, (capacity - m->count) * sizeof(record_t));
		}

		record_t *r = &m->records[m->count++];
		r->path = strdup(fields[0]);
		r->type = fields[1][0];
		r->mode = (uint32_t)strtoul(fields[2], NULL, 8);
		r->uid = (uint32_t)strtoul(fields[3], NULL, 10);
		r->gid = (uint32_t)strtoul(fields[4], NULL, 10);
		r->has_size = strcmp(fields[5], "-") != 0;
		r->size = r->has_size ? strtoull(fields[5], NULL, 10) : 0;
		r->has_nlink = strcmp(fields[6], "-") != 0;
		r->nlink = r->has_nlink ? (uint32_t)strtoul(fields[6], NULL, 10) : 0;
		r->has_ino = strcmp(fields[7], "-") != 0;
		if (r->has_ino) r->ino = strtoull(fields[7], NULL, 10);
		r->has_rdev = strcmp(fields[8], "-") != 0;
		if (r->has_rdev) sscanf(fields[8], "%u:%u", &r->rdev_major, &r->rdev_minor);
		r->has_mtime = strcmp(fields[9], "-") != 0;
		if (r->has_mtime) r->mtime = strtoull(fields[9], NULL, 10);
		r->has_crc = strcmp(fields[10], "-") != 0;
		if (r->has_crc) r->crc = (uint32_t)strtoul(fields[10], NULL, 16);
		if (strcmp(fields[11], "-") != 0) snprintf(r->sha, sizeof(r->sha), "%s", fields[11]);
		r->target = strcmp(fields[12], "-") != 0 ? strdup(fields[12]) : NULL;
		r->flag = strcmp(fields[13], "-") != 0 ? strdup(fields[13]) : NULL;
	}

	free(line);
	fclose(f);
	qsort(m->records, m->count, sizeof(record_t), record_compare);
	return true;
}

static record_t *manifest_find(manifest_t *m, const char *path)
{
	record_t key = { .path = (char *)path };
	return bsearch(&key, m->records, m->count, sizeof(record_t), record_compare);
}

static void manifest_free(manifest_t *m)
{
	for (size_t i = 0; i < m->count; i++) {
		free(m->records[i].path);
		free(m->records[i].target);
		free(m->records[i].flag);
	}
	free(m->records);
}

/* ---- deterministic randomness ----------------------------------------------------------------------- */

typedef struct { uint64_t state; } rng_t;

static uint64_t rng_next(rng_t *r)
{
	uint64_t x = r->state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	r->state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static uint64_t rng_below(rng_t *r, uint64_t bound)
{
	return bound == 0 ? 0 : rng_next(r) % bound;
}

/* ---- the walk --------------------------------------------------------------------------------------------- */

typedef struct {
	btrfs_fs_t *fs;
	image_t *image;
	manifest_t *manifest;    /* NULL: no comparison (sweeps) */
	bool sweep;              /* stop at the first error, print nothing */
	bool verbose;
	rng_t rng;

	uint64_t digest;         /* over everything observed */
	uint64_t entries, files, bytes, subvol_crossings, placeholders, partial_reads, lookups;
	uint64_t node_budget;
	int failures;
} walk_t;

#define MAX_DEPTH 64
#define MAX_PATH 4096
#define MAX_READ (32ULL * 1024 * 1024)

static void digest_mix(walk_t *w, const void *data, size_t length)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < length; i++) {
		w->digest ^= p[i];
		w->digest *= 0x100000001B3ULL;
	}
}

static void digest_u64(walk_t *w, uint64_t v)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
	digest_mix(w, b, 8);
}

static void fail(walk_t *w, const char *path, const char *format, ...)
{
	w->failures++;

	if (w->sweep) return;

	va_list args;
	va_start(args, format);
	fprintf(stderr, "FAIL %s: ", path);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static const char *type_name(uint32_t t)
{
	switch (t) {
	case BTRFS_S_IFREG: return "file";
	case BTRFS_S_IFDIR: return "dir";
	case BTRFS_S_IFLNK: return "symlink";
	case BTRFS_S_IFCHR: return "chardev";
	case BTRFS_S_IFBLK: return "blockdev";
	case BTRFS_S_IFIFO: return "fifo";
	case BTRFS_S_IFSOCK: return "socket";
	default: return "?";
	}
}

static char type_char(uint32_t t)
{
	switch (t) {
	case BTRFS_S_IFREG: return 'f';
	case BTRFS_S_IFDIR: return 'd';
	case BTRFS_S_IFLNK: return 'l';
	case BTRFS_S_IFCHR: return 'c';
	case BTRFS_S_IFBLK: return 'b';
	case BTRFS_S_IFIFO: return 'p';
	case BTRFS_S_IFSOCK: return 's';
	default: return '?';
	}
}

static uint8_t expected_ft(uint32_t t)
{
	switch (t) {
	case BTRFS_S_IFREG: return BTRFS_FT_REG_FILE;
	case BTRFS_S_IFDIR: return BTRFS_FT_DIR;
	case BTRFS_S_IFLNK: return BTRFS_FT_SYMLINK;
	case BTRFS_S_IFCHR: return BTRFS_FT_CHRDEV;
	case BTRFS_S_IFBLK: return BTRFS_FT_BLKDEV;
	case BTRFS_S_IFIFO: return BTRFS_FT_FIFO;
	case BTRFS_S_IFSOCK: return BTRFS_FT_SOCK;
	default: return BTRFS_FT_UNKNOWN;
	}
}

typedef struct {
	uint32_t expected;
	uint32_t seen;
} refs_check_t;

static bool count_ref(void *context, const btrfs_inode_ref_t *ref)
{
	refs_check_t *check = context;

	(void)ref;
	check->seen++;
	return true;
}

/* Read [offset, offset+length) and compare with the full read `whole`. */
static bool partial_read(walk_t *w, const btrfs_tree_t *tree, const btrfs_inode_t *inode, const uint8_t *whole, uint64_t offset, uint64_t length, const char *path)
{
	uint8_t *buffer = malloc(length ? length : 1);
	uint64_t done = 0;
	uint64_t size = inode->item.size;

	memset(buffer, 0xA5, length ? length : 1);

	btrfs_status_t status = btrfs_file_read(w->fs, tree, inode, offset, buffer, length, &done);
	uint64_t want = offset >= size ? 0 : (offset + length > size ? size - offset : length);
	bool ok = status == BTRFS_OK && done == want && (want == 0 || memcmp(buffer, whole + offset, (size_t)want) == 0);

	if (!ok) fail(w, path, "partial read [%llu,+%llu) status %s done %llu want %llu", (unsigned long long)offset, (unsigned long long)length, btrfs_status_name(status), (unsigned long long)done, (unsigned long long)want);

	w->partial_reads++;
	free(buffer);
	return ok;
}

static void check_file_contents(walk_t *w, const btrfs_tree_t *tree, const btrfs_inode_t *inode, const record_t *rec, const char *path)
{
	uint64_t size = inode->item.size;

	if (size > MAX_READ) {
		fail(w, path, "file too large for the harness (%llu)", (unsigned long long)size);
		return;
	}

	uint8_t *whole = malloc(size ? (size_t)size : 1);
	uint64_t done = 0;

	memset(whole, 0xA5, size ? (size_t)size : 1);

	if ((inode->item.flags & BTRFS_INODE_NODATASUM) != 0 && w->image != NULL) w->image->tag = 1;
	btrfs_status_t status = btrfs_file_read(w->fs, tree, inode, 0, whole, size, &done);
	if (w->image != NULL) w->image->tag = 0;

	if (rec != NULL && rec->flag != NULL && strcmp(rec->flag, "compressed") == 0) {
		/* The manifest says this file is compressed: it must be refused, cleanly. */
		if (status != BTRFS_ERR_UNSUPPORTED_COMPRESSION) fail(w, path, "compressed file read returned %s, expected unsupported compression", btrfs_status_name(status));
		else if (done != 0) fail(w, path, "a refused read still reported %llu bytes", (unsigned long long)done);

		/* Partial reads must be refused too, never served with garbage. */
		if (size > 0) {
			uint8_t probe[64];
			uint64_t got = 0;
			btrfs_status_t partial = btrfs_file_read(w->fs, tree, inode, size / 2, probe, sizeof(probe), &got);
			if (partial != BTRFS_ERR_UNSUPPORTED_COMPRESSION || got != 0) fail(w, path, "partial read of a compressed file returned %s", btrfs_status_name(partial));
		}

		digest_mix(w, path, strlen(path));
		free(whole);
		return;
	}

	if (status != BTRFS_OK || done != size) {
		fail(w, path, "read failed: %s (%llu of %llu bytes)", btrfs_status_name(status), (unsigned long long)done, (unsigned long long)size);
		free(whole);
		return;
	}

	uint32_t crc = btrfs_csum_crc32c(whole, (size_t)size);
	w->files++;
	w->bytes += size;
	digest_u64(w, crc);
	digest_u64(w, size);

	if (rec != NULL) {
		if (rec->has_crc && crc != rec->crc) fail(w, path, "crc32c %08x, Linux says %08x", crc, rec->crc);

		if (rec->sha[0] != '\0') {
			sha256_t sha;
			char hex[65];

			sha256_init(&sha);
			sha256_update(&sha, whole, (size_t)size);
			sha256_hex(&sha, hex);
			if (strcmp(hex, rec->sha) != 0) fail(w, path, "sha256 %s, Linux says %s", hex, rec->sha);
		}
	}

	/* Random partial reads, EOF cases and odd-sized steps, all checked against the full read. */
	if (!w->sweep && w->failures == 0) {
		partial_read(w, tree, inode, whole, size, 10, path);
		partial_read(w, tree, inode, whole, size + 4096, 10, path);
		partial_read(w, tree, inode, whole, size > 5 ? size - 5 : 0, 100, path);
		partial_read(w, tree, inode, whole, 0, 0, path);

		for (int i = 0; i < 6 && size > 0; i++) {
			uint64_t offset = rng_below(&w->rng, size);
			uint64_t max = size - offset;
			uint64_t length = 1 + rng_below(&w->rng, max < 70000 ? max + 100 : 70000);
			partial_read(w, tree, inode, whole, offset, length, path);
		}

		if (size > 0 && size <= 4 * 1024 * 1024) {
			uint8_t *stitched = malloc((size_t)size);
			uint64_t position = 0;

			while (position < size) {
				uint64_t got = 0;
				btrfs_status_t s = btrfs_file_read(w->fs, tree, inode, position, stitched + position, 4097, &got);
				if (s != BTRFS_OK || got == 0) { fail(w, path, "stepped read stalled at %llu: %s", (unsigned long long)position, btrfs_status_name(s)); break; }
				position += got;
			}

			if (position == size && memcmp(stitched, whole, (size_t)size) != 0) fail(w, path, "stepped read differs from the full read");
			free(stitched);
		}
	}

	free(whole);
}

static void check_node(walk_t *w, const btrfs_tree_t *tree, const btrfs_inode_t *inode, const char *path, uint8_t dirent_type, uint64_t dirent_ino)
{
	uint32_t type = btrfs_inode_type(inode->item.mode);
	record_t *rec = w->manifest != NULL ? manifest_find(w->manifest, path) : NULL;

	w->entries++;
	digest_mix(w, path, strlen(path));
	digest_u64(w, inode->item.mode);
	digest_u64(w, inode->item.size);
	digest_u64(w, inode->item.nlink);
	digest_u64(w, inode->item.uid);
	digest_u64(w, inode->item.gid);

	if (dirent_type != expected_ft(type)) fail(w, path, "directory entry type %u does not match inode mode %o", dirent_type, inode->item.mode);
	(void)dirent_ino;

	if (w->manifest != NULL) {
		if (rec == NULL) {
			fail(w, path, "not in the manifest (the driver shows an entry Linux does not)");
		} else {
			rec->visited = true;
			if (rec->type != type_char(type)) fail(w, path, "type %s, Linux says '%c'", type_name(type), rec->type);
			if (rec->mode != inode->item.mode) fail(w, path, "mode %o, Linux says %o", inode->item.mode, rec->mode);
			if (rec->uid != inode->item.uid || rec->gid != inode->item.gid) fail(w, path, "owner %u:%u, Linux says %u:%u", inode->item.uid, inode->item.gid, rec->uid, rec->gid);
			if (rec->has_size && rec->size != inode->item.size) fail(w, path, "size %llu, Linux says %llu", (unsigned long long)inode->item.size, (unsigned long long)rec->size);
			if (rec->has_nlink && rec->nlink != inode->item.nlink) fail(w, path, "nlink %u, Linux says %u", inode->item.nlink, rec->nlink);
			if (rec->has_ino && rec->ino != inode->ino) fail(w, path, "inode %llu, Linux says %llu", (unsigned long long)inode->ino, (unsigned long long)rec->ino);
			if (rec->has_mtime && rec->mtime != inode->item.mtime.sec) fail(w, path, "mtime %llu, Linux says %llu", (unsigned long long)inode->item.mtime.sec, (unsigned long long)rec->mtime);

			if (type == BTRFS_S_IFCHR || type == BTRFS_S_IFBLK) {
				uint64_t rdev = inode->item.rdev;
				/* Btrfs stores the kernel's dev_t: major << 20 | minor. */
				uint32_t major = (uint32_t)(rdev >> 20);
				uint32_t minor = (uint32_t)(rdev & 0xfffffU);
				if (!rec->has_rdev || rec->rdev_major != major || rec->rdev_minor != minor) fail(w, path, "device %u:%u, Linux says %u:%u", major, minor, rec->rdev_major, rec->rdev_minor);
			}
		}
	}

	/* Names: the way back from the inode. Hard links must match nlink. */
	if (!w->sweep && type != BTRFS_S_IFDIR && inode->item.nlink > 1) {
		refs_check_t check = { inode->item.nlink, 0 };
		btrfs_status_t s = btrfs_inode_refs(w->fs, tree, inode->ino, count_ref, &check, NULL);
		if (s != BTRFS_OK || check.seen != check.expected) fail(w, path, "%u names found (%s), nlink is %u", check.seen, btrfs_status_name(s), check.expected);
	}

	if (type == BTRFS_S_IFREG) {
		check_file_contents(w, tree, inode, rec, path);
	} else if (type == BTRFS_S_IFLNK) {
		char target[MAX_PATH + 1];
		size_t length = 0;
		btrfs_status_t s = btrfs_file_readlink(w->fs, tree, inode, target, MAX_PATH, &length);

		if (s != BTRFS_OK) {
			fail(w, path, "readlink failed: %s", btrfs_status_name(s));
		} else {
			target[length] = '\0';
			digest_mix(w, target, length);
			if (rec != NULL && (rec->target == NULL || strlen(rec->target) != length || memcmp(rec->target, target, length) != 0)) fail(w, path, "symlink target differs from Linux's");
		}
	}
}

static void walk_directory(walk_t *w, const btrfs_tree_t *tree, uint64_t dir, const char *path, int depth);

/* Descend into one entry (a file, directory or subvolume point) of `tree`. */
static void walk_entry(walk_t *w, const btrfs_tree_t *tree, uint64_t dir, const btrfs_dirent_t *entry, const char *path, int depth)
{
	btrfs_inode_t inode;
	btrfs_subvol_t sub;
	const btrfs_tree_t *child_tree = tree;
	uint64_t ino;

	if (btrfs_dirent_is_subvol(entry)) {
		btrfs_status_t s = btrfs_root_resolve_point(w->fs, tree->objectid, dir, entry->name, entry->name_len, entry->location.objectid, &sub);

		if (s == BTRFS_ERR_NOT_FOUND) {
			/* A placeholder from a snapshot: an empty directory, as Linux shows it. */
			memset(&inode, 0, sizeof(inode));
			inode.ino = 2;
			inode.item.mode = BTRFS_S_IFDIR | 0755U;
			inode.item.nlink = 1;
			check_node(w, tree, &inode, path, entry->type, 2);
			w->placeholders++;
			return;
		}

		if (s != BTRFS_OK) {
			fail(w, path, "subvolume %llu: %s", (unsigned long long)entry->location.objectid, btrfs_status_name(s));
			return;
		}

		child_tree = &sub.tree;
		ino = sub.item.root_dirid;
		w->subvol_crossings++;
	} else {
		ino = entry->location.objectid;
	}

	btrfs_status_t s = btrfs_inode_read(w->fs, child_tree, ino, &inode);
	if (s != BTRFS_OK) {
		fail(w, path, "inode %llu: %s", (unsigned long long)ino, btrfs_status_name(s));
		return;
	}

	check_node(w, child_tree, &inode, path, entry->type, ino);

	if (btrfs_inode_type(inode.item.mode) == BTRFS_S_IFDIR && (!w->sweep || w->failures == 0)) walk_directory(w, child_tree, ino, path, depth + 1);
}

static void walk_directory(walk_t *w, const btrfs_tree_t *tree, uint64_t dir, const char *path, int depth)
{
	if (depth > MAX_DEPTH) { fail(w, path, "directory nesting too deep (a loop?)"); return; }

	uint64_t cursor = 0;
	uint64_t count = 0;
	uint64_t previous_index = 0;
	btrfs_dirent_t entry;

	for (;;) {
		btrfs_status_t s = btrfs_dir_next(w->fs, tree, dir, &cursor, &entry);

		if (s == BTRFS_ERR_END) break;
		if (s != BTRFS_OK) { fail(w, path, "readdir: %s", btrfs_status_name(s)); return; }

		if (w->node_budget-- == 0) { fail(w, path, "node budget exhausted (a directory loop?)"); return; }
		if (count != 0 && entry.index <= previous_index) { fail(w, path, "DIR_INDEX did not advance"); return; }
		previous_index = entry.index;
		count++;

		char child[MAX_PATH];
		int n = snprintf(child, sizeof(child), "%s%s%s", path, strcmp(path, "/") == 0 ? "" : "/", entry.name);
		if (n < 0 || n >= (int)sizeof(child)) { fail(w, path, "path too long"); return; }

		/* The name must also be found by hash, and must lead to the same place. */
		btrfs_dirent_t found;
		s = btrfs_dir_lookup(w->fs, tree, dir, entry.name, entry.name_len, &found);
		w->lookups++;
		if (s != BTRFS_OK || found.location.objectid != entry.location.objectid || found.location.type != entry.location.type || found.type != entry.type) {
			fail(w, child, "lookup by name: %s", btrfs_status_name(s));
			return;
		}

		walk_entry(w, tree, dir, &entry, child, depth);
		if (w->sweep && w->failures != 0) return;
	}

	/* Names that are not there must not be found. */
	if (!w->sweep && w->failures == 0) {
		btrfs_dirent_t missing;
		const uint8_t nope[] = "this-name-does-not-exist";
		btrfs_status_t s = btrfs_dir_lookup(w->fs, tree, dir, nope, sizeof(nope) - 1, &missing);
		if (s != BTRFS_ERR_NOT_FOUND) fail(w, path, "lookup of a missing name returned %s", btrfs_status_name(s));

		/* Resuming from any saved cursor continues exactly after the entry it was saved at. */
		uint64_t saved_cursor = 0;
		uint64_t index = 0;
		btrfs_dirent_t first, again;

		if (count > 3) {
			cursor = 0;
			for (uint64_t skip = 0; skip < count / 2; skip++) {
				if (btrfs_dir_next(w->fs, tree, dir, &cursor, &first) != BTRFS_OK) break;
				index = first.index;
			}
			saved_cursor = cursor;

			btrfs_dirent_t next_a, next_b;
			uint64_t ca = saved_cursor, cb = saved_cursor;
			s = btrfs_dir_next(w->fs, tree, dir, &ca, &next_a);
			btrfs_status_t s2 = btrfs_dir_next(w->fs, tree, dir, &cb, &next_b);
			if (s != BTRFS_OK || s2 != BTRFS_OK || next_a.index != next_b.index || next_a.index <= index || strcmp((char *)next_a.name, (char *)next_b.name) != 0) fail(w, path, "readdir cursor resume is not stable");

			/* Resuming at an index past the last entry ends the directory. */
			uint64_t beyond = previous_index + 1;
			s = btrfs_dir_next(w->fs, tree, dir, &beyond, &again);
			if (s != BTRFS_ERR_END) fail(w, path, "readdir past the last index returned %s", btrfs_status_name(s));
		}
	}
}

/* Walk the mounted subvolume from its root inode. */
static void walk_mount(walk_t *w)
{
	btrfs_subvol_t root;
	btrfs_status_t s = btrfs_root_lookup(w->fs, w->fs->mount_subvol, &root);

	if (s != BTRFS_OK) { fail(w, "/", "mount subvolume: %s", btrfs_status_name(s)); return; }

	btrfs_inode_t inode;
	s = btrfs_inode_read(w->fs, &root.tree, root.item.root_dirid, &inode);
	if (s != BTRFS_OK) { fail(w, "/", "root inode: %s", btrfs_status_name(s)); return; }

	if (btrfs_inode_type(inode.item.mode) != BTRFS_S_IFDIR) { fail(w, "/", "the root is not a directory"); return; }

	check_node(w, &root.tree, &inode, "/", BTRFS_FT_DIR, root.item.root_dirid);
	walk_directory(w, &root.tree, root.item.root_dirid, "/", 0);
}

/* ---- commands ---------------------------------------------------------------------------------------------------- */

typedef struct {
	btrfs_open_options_t options;
	bool verbose;
	uint64_t seed;
	uint64_t iterations;
} options_t;

static btrfs_fs_t *open_fs(image_t *image, btrfs_reader_t *reader, btrfs_env_t *env, const options_t *opt, btrfs_status_t *status)
{
	(void)image;
	return btrfs_fs_open(env, reader, &opt->options, status);
}

static int cmd_info(const char *path, const options_t *opt)
{
	image_t image;
	btrfs_reader_t reader;
	btrfs_env_t env;
	btrfs_host_env_t host;
	btrfs_status_t status;

	if (!image_open(&image, &reader, path)) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	btrfs_host_env_init(&host, &env);
	host.verbose = opt->verbose;

	btrfs_fs_t *fs = open_fs(&image, &reader, &env, opt, &status);
	if (fs == NULL) { printf("open=%s\n", btrfs_status_name(status)); image_close(&image, &reader); return 3; }

	char features[128];
	btrfs_feature_names(fs->super.incompat_flags, fs->super.compat_ro_flags, features, sizeof(features));

	printf("label=%s\n", fs->super.label);
	printf("generation=%llu\n", (unsigned long long)fs->super.generation);
	printf("root=%llu\nroot_level=%u\n", (unsigned long long)fs->super.root, fs->super.root_level);
	printf("chunk_root=%llu\nchunk_root_level=%u\n", (unsigned long long)fs->super.chunk_root, fs->super.chunk_root_level);
	printf("total_bytes=%llu\nbytes_used=%llu\n", (unsigned long long)fs->super.total_bytes, (unsigned long long)fs->super.bytes_used);
	printf("sectorsize=%u\nnodesize=%u\nstripesize=%u\n", fs->super.sectorsize, fs->super.nodesize, fs->super.stripesize);
	printf("csum_type=%u\nnum_devices=%llu\n", fs->super.csum_type, (unsigned long long)fs->super.num_devices);
	printf("incompat=%llx\ncompat_ro=%llx\nfeatures=%s\n", (unsigned long long)fs->super.incompat_flags, (unsigned long long)fs->super.compat_ro_flags, features);
	printf("chunks=%u\ndefault_subvol=%llu\nmount_subvol=%llu\nsuper_copies_valid=%u\n", fs->chunks.count, (unsigned long long)fs->default_subvol, (unsigned long long)fs->mount_subvol, fs->super_copies_valid);

	btrfs_subvol_t sub;
	if (btrfs_root_lookup(fs, fs->mount_subvol, &sub) == BTRFS_OK) printf("mount_tree_level=%u\nmount_tree_bytenr=%llu\n", sub.tree.level, (unsigned long long)sub.tree.bytenr);

	for (uint32_t i = 0; i < fs->chunks.count; i++) {
		const btrfs_chunk_t *c = &fs->chunks.chunks[i];
		printf("chunk=%llu,%llu,%s,%s", (unsigned long long)c->logical, (unsigned long long)c->length, btrfs_profile_name(c->type), (c->type & BTRFS_BLOCK_GROUP_DATA) ? ((c->type & BTRFS_BLOCK_GROUP_METADATA) ? "data+metadata" : "data") : ((c->type & BTRFS_BLOCK_GROUP_SYSTEM) ? "system" : "metadata"));
		for (uint32_t st = 0; st < c->num_stripes; st++) printf(",%llu", (unsigned long long)c->stripes[st].offset);
		printf("\n");
	}

	btrfs_fs_close(fs);
	int leaks = (int)host.live_allocations;
	image_close(&image, &reader);
	if (leaks != 0) { fprintf(stderr, "LEAK: %d allocations live after close\n", leaks); return 1; }
	return 0;
}

static int cmd_open(const char *path, const options_t *opt)
{
	image_t image;
	btrfs_reader_t reader;
	btrfs_env_t env;
	btrfs_host_env_t host;
	btrfs_status_t status;

	if (!image_open(&image, &reader, path)) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	btrfs_host_env_init(&host, &env);
	host.verbose = opt->verbose;

	btrfs_fs_t *fs = open_fs(&image, &reader, &env, opt, &status);
	printf("open=%s\n", btrfs_status_name(status));
	if (fs != NULL) btrfs_fs_close(fs);

	int leaks = (int)host.live_allocations;
	image_close(&image, &reader);
	if (leaks != 0) { fprintf(stderr, "LEAK: %d allocations live after open/close\n", leaks); return 1; }
	return fs != NULL ? 0 : 3;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash != NULL ? slash + 1 : path;
}

/* Walk the tree without a manifest; failures print their status on stderr. */
static int cmd_walk(const char *path, const options_t *opt)
{
	image_t image;
	btrfs_reader_t reader;
	btrfs_env_t env;
	btrfs_host_env_t host;
	btrfs_status_t status;

	if (!image_open(&image, &reader, path)) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	btrfs_host_env_init(&host, &env);
	host.verbose = opt->verbose;

	btrfs_fs_t *fs = open_fs(&image, &reader, &env, opt, &status);
	if (fs == NULL) { printf("open=%s\n", btrfs_status_name(status)); image_close(&image, &reader); return 3; }

	walk_t w;
	memset(&w, 0, sizeof(w));
	w.fs = fs;
	w.image = &image;
	w.digest = 0xCBF29CE484222325ULL;
	w.node_budget = 10 * 1000 * 1000;
	w.rng.state = 12345;
	walk_mount(&w);

	printf("walk=%s entries=%llu files=%llu mirror_fallbacks=%llu csum_failures=%llu\n", w.failures == 0 ? "ok" : "fail", (unsigned long long)w.entries, (unsigned long long)w.files, (unsigned long long)fs->stats.mirror_fallbacks, (unsigned long long)fs->stats.csum_failures);
	btrfs_fs_close(fs);

	int leaks = (int)host.live_allocations;
	image_close(&image, &reader);
	if (leaks != 0) { fprintf(stderr, "LEAK: %d allocations live after close\n", leaks); return 1; }
	return w.failures == 0 ? 0 : 1;
}

/* List every subvolume with its parent and name (ROOT_BACKREF), for comparison with `btrfs subvolume list`. */
static int cmd_subvols(const char *path, const options_t *opt)
{
	image_t image;
	btrfs_reader_t reader;
	btrfs_env_t env;
	btrfs_host_env_t host;
	btrfs_status_t status;

	if (!image_open(&image, &reader, path)) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	btrfs_host_env_init(&host, &env);
	host.verbose = opt->verbose;

	btrfs_fs_t *fs = open_fs(&image, &reader, &env, opt, &status);
	if (fs == NULL) { printf("open=%s\n", btrfs_status_name(status)); image_close(&image, &reader); return 3; }

	uint64_t cursor = 0;
	uint64_t id;
	int rc = 0;

	while ((status = btrfs_root_next_subvol(fs, &cursor, &id)) == BTRFS_OK) {
		btrfs_root_ref_info_t ref;
		btrfs_subvol_t sub;

		if (btrfs_root_lookup(fs, id, &sub) != BTRFS_OK) { rc = 1; break; }

		if (id == BTRFS_FS_TREE_OBJECTID) {
			printf("subvol id=%llu parent=0 name=- readonly=%d\n", (unsigned long long)id, sub.read_only);
		} else if (btrfs_root_backref(fs, id, &ref) == BTRFS_OK) {
			printf("subvol id=%llu parent=%llu name=%s readonly=%d\n", (unsigned long long)id, (unsigned long long)ref.parent_id, ref.name, sub.read_only);
		} else {
			printf("subvol id=%llu parent=? name=? readonly=%d\n", (unsigned long long)id, sub.read_only);
		}
	}

	if (status != BTRFS_ERR_END) rc = 1;
	btrfs_fs_close(fs);
	int leaks = (int)host.live_allocations;
	image_close(&image, &reader);
	if (leaks != 0) { fprintf(stderr, "LEAK: %d allocations live after close\n", leaks); return 1; }
	return rc;
}

static int cmd_check(const char *path, const char *manifest_path, const options_t *opt)
{
	image_t image;
	btrfs_reader_t reader;
	btrfs_env_t env;
	btrfs_host_env_t host;
	btrfs_status_t status;
	manifest_t manifest;

	if (!image_open(&image, &reader, path)) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	if (!manifest_load(&manifest, manifest_path)) { fprintf(stderr, "cannot read manifest %s\n", manifest_path); return 2; }

	btrfs_host_env_init(&host, &env);
	host.verbose = opt->verbose;

	btrfs_fs_t *fs = open_fs(&image, &reader, &env, opt, &status);
	if (fs == NULL) {
		printf("FAIL %s: open failed: %s\n", base_name(path), btrfs_status_name(status));
		return 1;
	}

	walk_t w;
	memset(&w, 0, sizeof(w));
	w.fs = fs;
	w.image = &image;
	w.manifest = &manifest;
	w.verbose = opt->verbose;
	w.rng.state = opt->seed * 0x9E3779B97F4A7C15ULL + 88172645463325252ULL;
	w.digest = 0xCBF29CE484222325ULL;
	w.node_budget = 10 * 1000 * 1000;

	walk_mount(&w);

	for (size_t i = 0; i < manifest.count; i++) {
		if (!manifest.records[i].visited) {
			fail(&w, manifest.records[i].path, "in Linux's manifest but not reached by the driver");
		}
	}

	uint64_t verified = fs->stats.data_csum_checked;
	uint64_t csum_failures = fs->stats.csum_failures;
	btrfs_fs_close(fs);

	if (host.live_allocations != 0) {
		fprintf(stderr, "LEAK: %llu allocations live after close\n", (unsigned long long)host.live_allocations);
		w.failures++;
	}

	printf("%s %s: %llu entries, %llu files, %llu bytes, %llu subvolume crossings, %llu placeholders, %llu lookups, %llu partial reads, %llu data sectors verified, %llu csum failures\n",
		w.failures == 0 ? "PASS" : "FAIL", base_name(path), (unsigned long long)w.entries, (unsigned long long)w.files, (unsigned long long)w.bytes,
		(unsigned long long)w.subvol_crossings, (unsigned long long)w.placeholders, (unsigned long long)w.lookups, (unsigned long long)w.partial_reads, (unsigned long long)verified, (unsigned long long)csum_failures);

	manifest_free(&manifest);
	image_close(&image, &reader);
	return w.failures == 0 ? 0 : 1;
}

/* ---- the corruption sweep ------------------------------------------------------------------------------------------------- */

static volatile sig_atomic_t g_alarm_fired;
static const char *g_sweep_case = "";

static void on_alarm(int sig)
{
	(void)sig;
	g_alarm_fired = 1;
	static const char message[] = "FAIL sweep: an iteration did not finish in time (a hang)\n";
	(void)!write(2, message, sizeof(message) - 1);
	_exit(4);
}

typedef struct {
	uint64_t open_failures, walk_failures, healed_or_unharmed;
	uint64_t status_counts[24];
	uint64_t by_kind[8];
	uint64_t bad;
} sweep_stats_t;

static const char *g_kind_names[8] = { "bitflip", "zero", "scribble", "truncate", "super-semantic", "block-semantic", "read-error", "alloc-failure" };

/* Recompute a tree block's checksum in a patch buffer. */
static void fix_block_csum(uint8_t *block, size_t size)
{
	btrfs_put_le32(block, btrfs_csum_crc32c(block + BTRFS_CSUM_SIZE, size - BTRFS_CSUM_SIZE));
}

static int cmd_sweep(const char *path, const options_t *opt)
{
	image_t image;
	btrfs_reader_t reader;
	btrfs_env_t env;
	btrfs_host_env_t host;
	btrfs_status_t status;
	options_t local = *opt;

	if (!image_open(&image, &reader, path)) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	btrfs_host_env_init(&host, &env);
	local.options.verify_data_csums = true;

	/* Baseline: what the good image produces, and which bytes it depends on. */
	image.record = true;
	btrfs_fs_t *fs = open_fs(&image, &reader, &env, &local, &status);
	if (fs == NULL) { printf("FAIL %s: the baseline does not open: %s\n", base_name(path), btrfs_status_name(status)); return 1; }

	walk_t base;
	memset(&base, 0, sizeof(base));
	base.fs = fs;
	base.image = &image;
	base.sweep = true;
	base.digest = 0xCBF29CE484222325ULL;
	base.node_budget = 10 * 1000 * 1000;
	walk_mount(&base);

	uint32_t nodesize = fs->nodesize;
	uint64_t super_offsets[3] = { BTRFS_SUPER_INFO_OFFSET, 64ULL << 20, 0 };
	btrfs_fs_close(fs);

	if (base.failures != 0) { printf("FAIL %s: the baseline walk failed\n", base_name(path)); return 1; }

	uint64_t baseline_reads = image.reads;
	uint64_t baseline_allocs = host.total_allocations;
	uint64_t baseline_digest = base.digest;

	/* Only bytes the driver actually read can matter; skip data nothing can protect. */
	size_t sample_count = 0;
	uint64_t sample_total = 0;
	range_t *samples = malloc(image.range_count * sizeof(range_t) + 1);
	range_t *blocks = malloc(image.range_count * sizeof(range_t) + 1);
	size_t block_count = 0;

	for (size_t i = 0; i < image.range_count; i++) {
		if (image.ranges[i].tag == 1) continue;
		samples[sample_count++] = image.ranges[i];
		sample_total += image.ranges[i].length;
		if (image.ranges[i].length == nodesize) blocks[block_count++] = image.ranges[i];
	}

	image.record = false;

	sweep_stats_t stats;
	memset(&stats, 0, sizeof(stats));
	rng_t rng = { opt->seed * 0x9E3779B97F4A7C15ULL + 1 };
	uint64_t file_size = image.file.size;

	signal(SIGALRM, on_alarm);

	for (uint64_t iteration = 0; iteration < opt->iterations; iteration++) {
		image_reset_damage(&image, &reader);
		host.fail_after = 0;

		int kind = (int)rng_below(&rng, 8);
		bool exact = true;   /* must the result equal the baseline if the mount succeeds? */

		stats.by_kind[kind]++;

		switch (kind) {
		case 0:
		case 1:
		case 2: {
			int hits = 1 + (int)rng_below(&rng, 3);

			for (int h = 0; h < hits; h++) {
				uint64_t pick = rng_below(&rng, sample_total);
				size_t r = 0;
				while (r + 1 < sample_count && pick >= samples[r].length) { pick -= samples[r].length; r++; }

				uint64_t at = samples[r].offset + pick;
				uint8_t bytes[64];
				size_t length = 1;

				if (kind == 0) {
					uint8_t original;
					btrfs_reader_read(&image.base, at, &original, 1);
					bytes[0] = (uint8_t)(original ^ (1U << rng_below(&rng, 8)));
				} else if (kind == 1) {
					length = 1 + (size_t)rng_below(&rng, 64);
					memset(bytes, 0, length);
				} else {
					length = 1 + (size_t)rng_below(&rng, 8);
					for (size_t b = 0; b < length; b++) bytes[b] = (uint8_t)rng_next(&rng);
				}

				if (at + length > file_size) length = (size_t)(file_size - at);
				if (length != 0) image_patch(&image, at, bytes, length);
			}
			break;
		}
		case 3: {
			uint64_t cut = rng_below(&rng, 2) ? rng_below(&rng, 16ULL << 20) : rng_below(&rng, file_size + 1);
			image.size = cut < file_size ? cut : file_size;
			reader.size = image.size;
			break;
		}
		case 4: {
			/* A superblock that still checksums correctly but lies: exercise the geometry checks. */
			exact = false;

			for (int copy = 0; copy < 2; copy++) {
				uint64_t at = super_offsets[copy];
				if (at + BTRFS_SUPER_INFO_SIZE > file_size) continue;

				uint8_t *raw = malloc(BTRFS_SUPER_INFO_SIZE);
				btrfs_reader_read(&image.base, at, raw, BTRFS_SUPER_INFO_SIZE);
				rng_t again = rng;
				int edits = 1 + (int)rng_below(&again, 4);

				for (int e = 0; e < edits; e++) {
					uint32_t position = BTRFS_CSUM_SIZE + (uint32_t)rng_below(&again, 900);
					if (rng_below(&again, 4) == 0) position = 811 + (uint32_t)rng_below(&again, 140);   /* the sys_chunk_array */
					raw[position] = (uint8_t)rng_next(&again);
				}

				fix_block_csum(raw, BTRFS_SUPER_INFO_SIZE);
				image_patch(&image, at, raw, BTRFS_SUPER_INFO_SIZE);
				free(raw);
			}

			(void)rng_next(&rng);
			break;
		}
		case 5: {
			/* A tree block that still checksums correctly but lies: exercise the structure checks. */
			exact = false;
			if (block_count == 0) break;

			const range_t *b = &blocks[rng_below(&rng, block_count)];
			uint8_t *raw = malloc(nodesize);
			btrfs_reader_read(&image.base, b->offset, raw, nodesize);
			int edits = 1 + (int)rng_below(&rng, 4);

			for (int e = 0; e < edits; e++) {
				uint32_t position = BTRFS_CSUM_SIZE + (uint32_t)rng_below(&rng, rng_below(&rng, 2) ? 160 : nodesize - BTRFS_CSUM_SIZE);
				raw[position] = (uint8_t)rng_next(&rng);
			}

			fix_block_csum(raw, nodesize);
			image_patch(&image, b->offset, raw, nodesize);
			free(raw);
			break;
		}
		case 6:
			image.fail_read_at = 1 + rng_below(&rng, baseline_reads + baseline_reads / 4 + 1);
			break;
		case 7:
			host.fail_after = 1 + rng_below(&rng, baseline_allocs + 1);
			break;
		}

		image.read_budget = 50 * baseline_reads + 2000;
		alarm(30);
		g_sweep_case = g_kind_names[kind];

		fs = open_fs(&image, &reader, &env, &local, &status);

		if (fs == NULL) {
			stats.open_failures++;
			if ((unsigned)status < 24) stats.status_counts[status]++;
		} else {
			walk_t w;
			memset(&w, 0, sizeof(w));
			w.fs = fs;
			w.image = &image;
			w.sweep = true;
			w.digest = 0xCBF29CE484222325ULL;
			w.node_budget = 10 * 1000 * 1000;

			walk_mount(&w);
			btrfs_fs_close(fs);

			if (w.failures != 0) {
				stats.walk_failures++;
			} else if (w.digest == baseline_digest) {
				stats.healed_or_unharmed++;
			} else if (exact) {
				printf("FAIL %s: iteration %llu (%s) succeeded with DIFFERENT data than the good image\n", base_name(path), (unsigned long long)iteration, g_kind_names[kind]);
				stats.bad++;
			} else {
				stats.healed_or_unharmed++;   /* a lying-but-checksummed structure: only safety is required */
			}
		}

		alarm(0);
		host.fail_after = 0;

		if (host.live_allocations != 0) {
			printf("FAIL %s: iteration %llu (%s) leaked %llu allocations\n", base_name(path), (unsigned long long)iteration, g_kind_names[kind], (unsigned long long)host.live_allocations);
			stats.bad++;
			host.live_allocations = 0;
			host.live_bytes = 0;
		}

		if (image.budget_exceeded) {
			/* A walk that keeps reading far beyond the good image's needs is a runaway loop. */
			printf("FAIL %s: iteration %llu (%s) exceeded its read budget (%llu reads)\n", base_name(path), (unsigned long long)iteration, g_kind_names[kind], (unsigned long long)image.reads);
			stats.bad++;
		}
	}

	printf("%s %s sweep: %llu iterations (", stats.bad == 0 ? "PASS" : "FAIL", base_name(path), (unsigned long long)opt->iterations);
	for (int k = 0; k < 8; k++) printf("%s%s %llu", k ? ", " : "", g_kind_names[k], (unsigned long long)stats.by_kind[k]);
	printf("); mount refused %llu, walk failed cleanly %llu, healed or unharmed %llu, bad %llu\n",
		(unsigned long long)stats.open_failures, (unsigned long long)stats.walk_failures, (unsigned long long)stats.healed_or_unharmed, (unsigned long long)stats.bad);

	free(samples);
	free(blocks);
	image_close(&image, &reader);
	return stats.bad == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
	options_t opt;
	const char *positional[4];
	int npos = 0;

	memset(&opt, 0, sizeof(opt));
	opt.iterations = 200;
	opt.seed = 1;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--subvol") == 0 && i + 1 < argc) opt.options.subvol_id = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--verify-data") == 0) opt.options.verify_data_csums = true;
		else if (strcmp(argv[i], "--ignore-log") == 0) opt.options.ignore_log_tree = true;
		else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) opt.seed = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) opt.iterations = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-v") == 0) opt.verbose = true;
		else if (npos < 4) positional[npos++] = argv[i];
	}

	if (argc >= 3 && strcmp(argv[1], "info") == 0 && npos == 1) return cmd_info(positional[0], &opt);
	if (argc >= 3 && strcmp(argv[1], "open") == 0 && npos == 1) return cmd_open(positional[0], &opt);
	if (argc >= 4 && strcmp(argv[1], "check") == 0 && npos == 2) return cmd_check(positional[0], positional[1], &opt);
	if (argc >= 3 && strcmp(argv[1], "sweep") == 0 && npos == 1) return cmd_sweep(positional[0], &opt);
	if (argc >= 3 && strcmp(argv[1], "walk") == 0 && npos == 1) return cmd_walk(positional[0], &opt);
	if (argc >= 3 && strcmp(argv[1], "subvols") == 0 && npos == 1) return cmd_subvols(positional[0], &opt);

	fprintf(stderr, "usage: btrfs_host info|open|walk|subvols|check|sweep IMAGE [MANIFEST] [--subvol N] [--verify-data] [--ignore-log] [--seed N] [--iters N] [-v]\n");
	return 2;
}
