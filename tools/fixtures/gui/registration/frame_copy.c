/* Count production frame-copy calls and verify packing and failure handling. */
#include <drivers/video/ui_frame_copy.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t calls, fail_at;
static uint64_t source_begin, source_size;

bool vm_copy_from_user(void *destination, uint64_t address, uint64_t bytes)
{
	calls++;
	if (calls == fail_at)
		return false;
	assert(address >= source_begin && bytes <= source_size);
	assert(address - source_begin <= source_size - bytes);
	memcpy(destination, (const void *)(uintptr_t)address, (size_t)bytes);
	return true;
}

int main(void)
{
	uint32_t *source = malloc(1920ULL * 1080 * 4);
	uint32_t *output = malloc(1920ULL * 1080 * 4);
	assert(source && output);
	for (uint32_t i = 0; i < 1920 * 1080; i++)
		source[i] = i;
	source_begin = (uint64_t)(uintptr_t)source;
	source_size = 1920ULL * 1080 * 4;
	assert(ui_frame_copy_from_user(output, source_begin, 1920, 1080, 1920));
	assert(calls == 1 && memcmp(output, source, (size_t)source_size) == 0);
	puts("PASS 1080p packed frame: one validated copy call instead of 1080 row calls");

	calls = 0;
	assert(ui_frame_copy_from_user(output, source_begin, 5, 3, 7));
	assert(calls == 3);
	for (uint32_t y = 0; y < 3; y++)
		for (uint32_t x = 0; x < 5; x++)
			assert(output[y * 5 + x] == source[y * 7 + x]);
	calls = 0;
	fail_at = 2;
	assert(!ui_frame_copy_from_user(output, source_begin, 5, 3, 7) && calls == 2);
	calls = 0;
	fail_at = 1;
	assert(!ui_frame_copy_from_user(output, source_begin, 5, 3, 5) && calls == 1);
	calls = 0;
	assert(!ui_frame_copy_from_user(output, UINT64_MAX - 3, 5, 3, 7));
	assert(!ui_frame_copy_from_user(output, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX));
	assert(!ui_frame_copy_from_user(output, source_begin, 5, 3, 4));
	assert(!ui_frame_copy_from_user(output, source_begin, 0, 3, 5));
	assert(calls == 0);
	free(source);
	free(output);
	puts("PASS padded rows, copy faults, dimensions and source-range overflow");
}
