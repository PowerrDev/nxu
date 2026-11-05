/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/loader/segment_cache.c
 *
 * See kern/loader/segment_cache.h for the shared-segment model and lifecycle.
 */

#include <kern/loader/segment_cache.h>

#include <kern/machine/cache.h>
#include <kern/machine/cpu.h>
#include <kern/memory/heap.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LOADER_SEGMENT_CACHE_MAX 128U

struct loader_segment_cache_entry {
	loader_segment_key_t key;
	uint64_t *physical_pages;
	uint64_t page_count;
	bool valid;
	bool ready;
};

static struct loader_segment_cache_entry g_segment_cache[LOADER_SEGMENT_CACHE_MAX];
static volatile uint32_t g_segment_cache_lock;

static void
segment_cache_lock(void)
{
	while (__atomic_exchange_n(&g_segment_cache_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void
segment_cache_unlock(void)
{
	__atomic_store_n(&g_segment_cache_lock, 0U, __ATOMIC_RELEASE);
}

static bool
segment_cache_key_equal(const loader_segment_key_t *a, const loader_segment_key_t *b)
{
	if (a->file_offset != b->file_offset || a->file_size != b->file_size || a->memory_size != b->memory_size ||
		a->file_identity != b->file_identity || a->flags != b->flags)
		return false;

	for (uint32_t index = 0U; index < LOADER_SEGMENT_CACHE_PATH_MAX; index++) {
		if (a->path[index] != b->path[index]) return false;
		if (a->path[index] == 0) break;
	}

	return true;
}

/* Hand a reserved-but-unpublished slot back to the free pool. The frames, if
 * any were allocated, are the caller's to release first. */
static void
segment_cache_free_slot(struct loader_segment_cache_entry *entry)
{
	segment_cache_lock();
	entry->physical_pages = 0;
	entry->page_count = 0ULL;
	entry->ready = false;
	entry->valid = false;
	segment_cache_unlock();
}

loader_segment_cache_result_t
loader_segment_cache_acquire(const loader_segment_key_t *key, uint64_t page_count,
							 loader_segment_cache_entry_t **out_entry)
{
	if (out_entry != 0) *out_entry = 0;

	if (key == 0 || out_entry == 0 || page_count == 0ULL) return LOADER_SEGMENT_CACHE_SKIP;

	/*
	 * Match or reserve under the lock so two CPUs loading the same image never
	 * both build it: the first turns a free slot into a not-ready reservation,
	 * and the second sees that reservation and maps privately instead (a rare,
	 * harmless miss of the share). The ready check and the reservation are the
	 * only work done holding the lock; the frames are allocated below without
	 * it, the same way vm_shm reserves a range under a lock and maps outside.
	 */
	segment_cache_lock();

	for (uint32_t index = 0U; index < LOADER_SEGMENT_CACHE_MAX; index++) {
		if (!g_segment_cache[index].valid || !segment_cache_key_equal(&g_segment_cache[index].key, key)) continue;

		bool ready = g_segment_cache[index].ready;
		segment_cache_unlock();

		if (!ready) return LOADER_SEGMENT_CACHE_SKIP;

		*out_entry = &g_segment_cache[index];
		return LOADER_SEGMENT_CACHE_HIT;
	}

	struct loader_segment_cache_entry *entry = 0;
	for (uint32_t index = 0U; index < LOADER_SEGMENT_CACHE_MAX; index++) {
		if (g_segment_cache[index].valid) continue;

		entry = &g_segment_cache[index];
		entry->key = *key;
		entry->physical_pages = 0;
		entry->page_count = 0ULL;
		entry->ready = false;
		entry->valid = true;
		break;
	}

	segment_cache_unlock();

	if (entry == 0) return LOADER_SEGMENT_CACHE_SKIP;

	uint64_t *pages = kmalloc(page_count * sizeof(uint64_t));
	if (pages == 0) {
		segment_cache_free_slot(entry);
		return LOADER_SEGMENT_CACHE_SKIP;
	}

	uint64_t allocated = 0ULL;
	for (; allocated < page_count; allocated++) {
		uint64_t kernel_address;
		if (!pmm_allocate_page(&pages[allocated])) break;
		if (!vmm_physical_to_higher_half(pages[allocated], &kernel_address)) {
			(void)pmm_free_page(pages[allocated]);
			break;
		}
		memset((void *)kernel_address, 0, PMM_PAGE_SIZE);
	}

	if (allocated != page_count) {
		for (uint64_t index = 0ULL; index < allocated; index++)
			(void)pmm_free_page(pages[index]);
		(void)kfree(pages);
		segment_cache_free_slot(entry);
		return LOADER_SEGMENT_CACHE_SKIP;
	}

	entry->physical_pages = pages;
	entry->page_count = page_count;

	*out_entry = entry;
	return LOADER_SEGMENT_CACHE_MISS;
}

uint64_t
loader_segment_cache_page(const loader_segment_cache_entry_t *entry, uint64_t index)
{
	if (entry == 0 || index >= entry->page_count) return 0ULL;

	uint64_t kernel_address;
	if (!vmm_physical_to_higher_half(entry->physical_pages[index], &kernel_address)) return 0ULL;

	return kernel_address;
}

uint64_t
loader_segment_cache_page_count(const loader_segment_cache_entry_t *entry)
{
	return entry == 0 ? 0ULL : entry->page_count;
}

void
loader_segment_cache_commit(loader_segment_cache_entry_t *entry)
{
	if (entry == 0) return;

	segment_cache_lock();
	entry->ready = true;
	segment_cache_unlock();
}

void
loader_segment_cache_abandon(loader_segment_cache_entry_t *entry)
{
	if (entry == 0) return;

	/* The frames of a MISS are mapped nowhere yet, so this drops the cache's
	 * only reference and frees them outright. */
	for (uint64_t index = 0ULL; index < entry->page_count; index++)
		(void)pmm_free_page(entry->physical_pages[index]);

	(void)kfree(entry->physical_pages);
	segment_cache_free_slot(entry);
}

bool
loader_segment_cache_map(const loader_segment_cache_entry_t *entry, vm_address_space_t *space, uint64_t base_va,
						 vm_user_protection_t protection)
{
	if (entry == 0 || space == 0) return false;

	uint64_t mapped = 0ULL;
	for (; mapped < entry->page_count; mapped++) {
		uint64_t page_va = base_va + mapped * PMM_PAGE_SIZE;

		if (!pmm_page_retain(entry->physical_pages[mapped])) break;

		if (!vm_address_space_map_page(space, page_va, entry->physical_pages[mapped], protection)) {
			(void)pmm_free_page(entry->physical_pages[mapped]);
			break;
		}
	}

	if (mapped != entry->page_count) {
		for (uint64_t index = 0ULL; index < mapped; index++) {
			uint64_t page_va = base_va + index * PMM_PAGE_SIZE;
			(void)vm_address_space_unmap_page(space, page_va);
			(void)pmm_free_page(entry->physical_pages[index]);
		}
		return false;
	}

	/* The frames carry a copy of the file's bytes, so code mapped from them
	 * needs the same instruction-cache sync a private load does -- harmless for
	 * read-only data, which never executes. */
	if (protection == VM_USER_PROTECTION_READ_EXECUTE) {
		for (uint64_t index = 0ULL; index < entry->page_count; index++) {
			uint64_t kernel_address;
			if (vmm_physical_to_higher_half(entry->physical_pages[index], &kernel_address))
				cache_sync_instruction_range(kernel_address, PMM_PAGE_SIZE);
		}
	}

	return true;
}
