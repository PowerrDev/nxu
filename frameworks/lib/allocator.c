#include <nxu/runtime.h>
#include <nxu/syscall.h>
#include <nxu/string.h>

#define ALLOCATION_MAGIC 0x4e5855414c4c4f43ULL

typedef struct {
	uint64_t magic, base, mapped, size;
} allocation_t;

void *nxu_alloc(uint64_t size, uint64_t alignment)
{
	if (!size || !alignment || alignment > 65536 || (alignment & (alignment - 1)))
		return 0;

	if (alignment < 16)
		alignment = 16;
	if (size > UINT64_MAX - alignment - sizeof(allocation_t) - 4095)
		return 0;

	uint64_t mapped = (size + alignment + sizeof(allocation_t) + 4095) & ~4095ULL;
	int64_t address = nxu_mmap(mapped, 0);

	if (address < 0)
		return 0;

	uint64_t pointer = ((uint64_t)address + sizeof(allocation_t) + alignment - 1) & ~(alignment - 1);
	allocation_t *header = (void *)(uintptr_t)(pointer - sizeof(allocation_t));
	*header = (allocation_t){ALLOCATION_MAGIC, (uint64_t)address, mapped, size};
	return (void *)(uintptr_t)pointer;
}

bool nxu_free(void *pointer)
{
	if (!pointer)
		return true;

	allocation_t *header = (void *)((uint8_t *)pointer - sizeof(allocation_t));

	if (header->magic != ALLOCATION_MAGIC)
		return false;

	uint64_t base = header->base, mapped = header->mapped;

	/* Retain the record if the syscall fails; callers may retry. */
	return nxu_munmap(base, mapped) == 0;
}

void *nxu_realloc(void *pointer, uint64_t size, uint64_t alignment)
{
	if (!pointer)
		return nxu_alloc(size, alignment);

	if (!size) {
		(void)nxu_free(pointer);
		return 0;
	}

	allocation_t *header = (void *)((uint8_t *)pointer - sizeof(allocation_t));

	if (header->magic != ALLOCATION_MAGIC)
		return 0;

	void *next = nxu_alloc(size, alignment);

	if (!next)
		return 0;

	memcpy(next, pointer, header->size < size ? header->size : size);
	if (!nxu_free(pointer)) {
		(void)nxu_free(next);
		return 0;
	}

	return next;
}

/* Compatibility imports used by the existing freestanding Rust allocator. */
void *kmalloc(uint64_t size)
{
	return nxu_alloc(size, 16);
}

bool kfree(void *pointer)
{
	return nxu_free(pointer);
}
