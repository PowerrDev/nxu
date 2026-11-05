/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/loader/segment_cache.h
 *
 * Shares the immutable (non-writable) PT_LOAD segments of an image across
 * every process that loads it, so a framework's text and read-only data live
 * in physical memory once instead of once per process. A segment is filled
 * from the file the first time it is seen and the very same physical frames
 * are then mapped into each later loader's address space -- NXU's own, much
 * smaller, take on the macOS dyld shared cache.
 *
 * Only R and R+X segments are shared. Writable (and GNU_RELRO) segments carry
 * per-process relocations, so the loader keeps mapping those privately; the
 * shared frames are therefore bias-independent and identical byte for byte no
 * matter where ASLR places the image, which is what makes them safe to share.
 *
 * Each frame is reference counted by the PMM (pmm_page_retain / pmm_free_page):
 * the cache holds one reference per frame for as long as the entry lives, and
 * every process that maps the segment holds one more, so a frame is returned to
 * the allocator only once the cache and all mappers have let go. Entries are
 * not evicted -- the system framework set is small and fixed for a session, the
 * same milestone-scoped simplification kern/ipc/shm_registry.c makes.
 */

#ifndef NXU_KERN_LOADER_SEGMENT_CACHE_H
#define NXU_KERN_LOADER_SEGMENT_CACHE_H

#include <vm/address_space.h>

#include <stdbool.h>
#include <stdint.h>

#define LOADER_SEGMENT_CACHE_PATH_MAX 256U

/*
 * What names one immutable segment. Two loads share frames only when every
 * field matches, so the key is deliberately bias-independent: the image path
 * and the segment's position and shape in the file, never the virtual address
 * ASLR chose. `file_identity` is the file's size at load time, a cheap guard
 * that a file replaced on disk between loads is not served stale frames.
 */
typedef struct {
	char path[LOADER_SEGMENT_CACHE_PATH_MAX];
	uint64_t file_offset;
	uint64_t file_size;
	uint64_t memory_size;
	uint64_t file_identity;
	uint32_t flags;
} loader_segment_key_t;

/* Opaque cache entry; a pointer to one stays valid once handed out (entries
 * are never freed), so the loader may hold it across the fill and the map. */
typedef struct loader_segment_cache_entry loader_segment_cache_entry_t;

typedef enum {
	/* `entry` is already filled: hash it and map it, nothing to load. */
	LOADER_SEGMENT_CACHE_HIT,
	/* `entry` is reserved with zeroed frames: fill it from the file, then
	 * loader_segment_cache_commit() and map it. */
	LOADER_SEGMENT_CACHE_MISS,
	/* Not cacheable right now (table full, a concurrent build, or no memory):
	 * the caller maps the segment privately, exactly as before this cache. */
	LOADER_SEGMENT_CACHE_SKIP,
} loader_segment_cache_result_t;

/*
 * loader_segment_cache_acquire
 *
 * Look `key` up. On a hit, returns the ready entry. On a miss, reserves a slot,
 * allocates and zeroes `page_count` frames for it, and returns it for the
 * caller to fill. Returns SKIP (and leaves `*out_entry` unset) when the segment
 * cannot be cached, so the caller falls back to a private mapping.
 */
loader_segment_cache_result_t loader_segment_cache_acquire(const loader_segment_key_t *key, uint64_t page_count,
														   loader_segment_cache_entry_t **out_entry);

/*
 * loader_segment_cache_page
 *
 * Higher-half kernel address of frame `index` of `entry`, so the loader can
 * read the file into it (on a miss) or hash it. 0 if `index` is out of range.
 */
uint64_t loader_segment_cache_page(const loader_segment_cache_entry_t *entry, uint64_t index);

/*
 * loader_segment_cache_page_count
 */
uint64_t loader_segment_cache_page_count(const loader_segment_cache_entry_t *entry);

/*
 * loader_segment_cache_commit
 *
 * Publish a MISS entry once its frames hold the file's bytes, so later loads
 * may share it. Call exactly once per MISS, before mapping.
 */
void loader_segment_cache_commit(loader_segment_cache_entry_t *entry);

/*
 * loader_segment_cache_abandon
 *
 * Give a MISS entry's slot and frames back when the fill fails, so a bad read
 * never leaves a half-built entry for another load to find.
 */
void loader_segment_cache_abandon(loader_segment_cache_entry_t *entry);

/*
 * loader_segment_cache_map
 *
 * Map every frame of `entry` into `space` from `base_va`, retaining each so the
 * space's teardown drops only its own reference. Executable segments get an
 * instruction-cache sync, the same one a private load performs. Unwinds its own
 * mappings and returns false on failure.
 */
bool loader_segment_cache_map(const loader_segment_cache_entry_t *entry, vm_address_space_t *space, uint64_t base_va,
							  vm_user_protection_t protection);

#endif
