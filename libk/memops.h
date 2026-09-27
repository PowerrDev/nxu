#ifndef NXU_LIBK_MEMOPS_H
#define NXU_LIBK_MEMOPS_H

/*
 * memcpy, memmove and memset a word at a time, shared by the kernel (libk)
 * and the userland C library (frameworks/lib/string.c), both of which only
 * define the public names around these.
 *
 * They used to move one byte per iteration. Every frame the desktop shows
 * goes through them several times (an app's frame into the kernel, into the
 * window's backing store, rows into the framebuffer), and so do the kernel's
 * copies to and from user memory: at 2732x1536 a byte loop was most of the
 * time a window drag took. Here whole words move when source and destination
 * are equally aligned (pixel rows always are, to 4 bytes), four per
 * iteration, with bytes only for the unaligned head and tail.
 *
 * Built -ffreestanding, so the compiler does not turn these loops back into
 * calls to memcpy.
 *
 * Every access is naturally aligned for its size, because the kernel copies
 * before its MMU is on, when all memory is Device memory and an unaligned
 * access faults. The byte and 32-bit loops go through relaxed atomics for
 * exactly that reason: the compiler may merge plain adjacent stores into a
 * wider one (two 32-bit stores into one 64-bit store at a 4-byte aligned
 * address), which is fine in normal memory and an alignment fault there.
 * It never merges atomic accesses. Word (8-byte) accesses are always
 * word-aligned here, so merged pairs of them stay aligned.
 */

#define NXU_MEMOPS_LOAD(pointer) __atomic_load_n((pointer), __ATOMIC_RELAXED)
#define NXU_MEMOPS_STORE(pointer, value) __atomic_store_n((pointer), (value), __ATOMIC_RELAXED)

#include <stddef.h>
#include <stdint.h>

typedef uintptr_t nxu_memops_word_t;

#define NXU_MEMOPS_WORD sizeof(nxu_memops_word_t)

static inline void *nxu_memops_copy_forward(void *destination, const void *source, size_t size)
{
	uint8_t *d = destination;
	const uint8_t *s = source;

	if ((((uintptr_t)d ^ (uintptr_t)s) & (NXU_MEMOPS_WORD - 1U)) == 0U) {
		while (size != 0U && ((uintptr_t)d & (NXU_MEMOPS_WORD - 1U)) != 0U) {
			NXU_MEMOPS_STORE(d, NXU_MEMOPS_LOAD(s));
			d++;
			s++;
			size--;
		}

		nxu_memops_word_t *dw = (nxu_memops_word_t *)(void *)d;
		const nxu_memops_word_t *sw = (const nxu_memops_word_t *)(const void *)s;

		while (size >= 4U * NXU_MEMOPS_WORD) {
			nxu_memops_word_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
			dw += 4;
			sw += 4;
			size -= 4U * NXU_MEMOPS_WORD;
		}
		while (size >= NXU_MEMOPS_WORD) {
			*dw++ = *sw++;
			size -= NXU_MEMOPS_WORD;
		}

		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	} else if ((((uintptr_t)d ^ (uintptr_t)s) & 3U) == 0U) {
		/* 32-bit pixels whose rows are only 4-byte aligned against each other. */
		while (size != 0U && ((uintptr_t)d & 3U) != 0U) {
			NXU_MEMOPS_STORE(d, NXU_MEMOPS_LOAD(s));
			d++;
			s++;
			size--;
		}

		uint32_t *dw = (uint32_t *)(void *)d;
		const uint32_t *sw = (const uint32_t *)(const void *)s;

		while (size >= 4U) {
			NXU_MEMOPS_STORE(dw, NXU_MEMOPS_LOAD(sw));
			dw++;
			sw++;
			size -= 4U;
		}

		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}

	while (size != 0U) {
		NXU_MEMOPS_STORE(d, NXU_MEMOPS_LOAD(s));
			d++;
			s++;
		size--;
	}

	return destination;
}

/* The same, from the end down: for an overlapping move to a higher address. */
static inline void *nxu_memops_copy_backward(void *destination, const void *source, size_t size)
{
	uint8_t *d = (uint8_t *)destination + size;
	const uint8_t *s = (const uint8_t *)source + size;

	if ((((uintptr_t)d ^ (uintptr_t)s) & (NXU_MEMOPS_WORD - 1U)) == 0U) {
		while (size != 0U && ((uintptr_t)d & (NXU_MEMOPS_WORD - 1U)) != 0U) {
			--d;
			--s;
			NXU_MEMOPS_STORE(d, NXU_MEMOPS_LOAD(s));
			size--;
		}

		nxu_memops_word_t *dw = (nxu_memops_word_t *)(void *)d;
		const nxu_memops_word_t *sw = (const nxu_memops_word_t *)(const void *)s;

		while (size >= NXU_MEMOPS_WORD) {
			*--dw = *--sw;
			size -= NXU_MEMOPS_WORD;
		}

		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}

	while (size != 0U) {
		--d;
			--s;
			NXU_MEMOPS_STORE(d, NXU_MEMOPS_LOAD(s));
		size--;
	}

	return destination;
}

static inline void *nxu_memops_move(void *destination, const void *source, size_t size)
{
	if (destination == source || size == 0U) return destination;

	/* Forward is safe unless the destination starts inside the source. */
	if ((uintptr_t)destination < (uintptr_t)source || (uintptr_t)destination >= (uintptr_t)source + size) {
		return nxu_memops_copy_forward(destination, source, size);
	}

	return nxu_memops_copy_backward(destination, source, size);
}

static inline void *nxu_memops_set(void *destination, int value, size_t size)
{
	uint8_t *d = destination;
	uint8_t byte = (uint8_t)value;

	while (size != 0U && ((uintptr_t)d & (NXU_MEMOPS_WORD - 1U)) != 0U) {
		NXU_MEMOPS_STORE(d, byte);
		d++;
		size--;
	}

	nxu_memops_word_t pattern = (nxu_memops_word_t)0x0101010101010101ULL * byte;
	nxu_memops_word_t *dw = (nxu_memops_word_t *)(void *)d;

	while (size >= 4U * NXU_MEMOPS_WORD) {
		dw[0] = pattern;
		dw[1] = pattern;
		dw[2] = pattern;
		dw[3] = pattern;
		dw += 4;
		size -= 4U * NXU_MEMOPS_WORD;
	}
	while (size >= NXU_MEMOPS_WORD) {
		*dw++ = pattern;
		size -= NXU_MEMOPS_WORD;
	}

	d = (uint8_t *)dw;
	while (size != 0U) {
		NXU_MEMOPS_STORE(d, byte);
		d++;
		size--;
	}

	return destination;
}

#endif
