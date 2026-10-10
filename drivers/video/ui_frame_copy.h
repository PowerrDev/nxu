#ifndef NXU_UI_FRAME_COPY_H
#define NXU_UI_FRAME_COPY_H

#include <vm/user_copy.h>
#include <stdint.h>

/* Pack app-owned rows without bypassing user-page validation. */
static inline bool ui_frame_copy_from_user(uint32_t *destination, uint64_t pixels, uint32_t width, uint32_t height,
										   uint32_t stride)
{
	if (!destination || !width || !height || stride < width)
		return false;

	uint64_t row_bytes = (uint64_t)width * sizeof(uint32_t);
	uint64_t stride_bytes = (uint64_t)stride * sizeof(uint32_t);
	uint64_t rows = (uint64_t)height - 1;

	if (rows > (UINT64_MAX - row_bytes) / stride_bytes)
		return false;

	uint64_t span = rows * stride_bytes + row_bytes;
	if (pixels > UINT64_MAX - span)
		return false;

	if (stride == width)
		return vm_copy_from_user(destination, pixels, span);

	for (uint32_t row = 0; row < height; row++)
		if (!vm_copy_from_user(destination + (uint64_t)row * width, pixels + (uint64_t)row * stride_bytes, row_bytes))
			return false;

	return true;
}

#endif
