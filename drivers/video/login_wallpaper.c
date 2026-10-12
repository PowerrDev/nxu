/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/login_wallpaper.c
 *
 * Decode and scale the embedded login wallpaper into the primary framebuffer.
 */
#include <drivers/video/login_wallpaper.h>

#include <libk/string.h>

#define LOGIN_WALLPAPER_MAX_WIDTH 4096U
#define LOGIN_WALLPAPER_MAX_HEIGHT 2160U

extern const uint8_t nxu_login_wallpaper_start[];
extern const uint8_t nxu_login_wallpaper_end[];

static uint32_t login_wallpaper_row[1920];
static uint32_t login_wallpaper_xmap[LOGIN_WALLPAPER_MAX_WIDTH];
static uint32_t login_wallpaper_ymap[LOGIN_WALLPAPER_MAX_HEIGHT];

static uint32_t
login_wallpaper_hash(uint8_t red, uint8_t green, uint8_t blue)
{
	return (red * 3U + green * 5U + blue * 7U + 255U * 11U) % 64U;
}

static bool
login_wallpaper_read_pixel(const uint8_t **cursor, const uint8_t *end, uint8_t *red, uint8_t *green, uint8_t *blue,
						   uint32_t *run, uint8_t index[64][4], uint32_t *pixel)
{
	if (*run > 0U) {
		(*run)--;
	} else {
		if (*cursor >= end) return false;

		uint8_t op = *(*cursor)++;

		if (op == 0xFEU) {
			if ((size_t)(end - *cursor) < 3U) return false;

			*red = *(*cursor)++;
			*green = *(*cursor)++;
			*blue = *(*cursor)++;
		} else if (op == 0xFFU) {
			if ((size_t)(end - *cursor) < 4U) return false;

			*red = *(*cursor)++;
			*green = *(*cursor)++;
			*blue = *(*cursor)++;
			(*cursor)++; /* The generated image is RGB, so alpha remains opaque. */
		} else if ((op & 0xC0U) == 0x00U) {
			*red = index[op & 0x3FU][0];
			*green = index[op & 0x3FU][1];
			*blue = index[op & 0x3FU][2];
		} else if ((op & 0xC0U) == 0x40U) {
			*red = (uint8_t)(*red + (int32_t)((op >> 4) & 3U) - 2);
			*green = (uint8_t)(*green + (int32_t)((op >> 2) & 3U) - 2);
			*blue = (uint8_t)(*blue + (int32_t)(op & 3U) - 2);
		} else if ((op & 0xC0U) == 0x80U) {
			if (*cursor >= end) return false;

			int32_t dg = (int32_t)(op & 0x3FU) - 32;
			uint8_t second = *(*cursor)++;
			int32_t dr = dg + (int32_t)(second >> 4) - 8;
			int32_t db = dg + (int32_t)(second & 0x0FU) - 8;
			*red = (uint8_t)(*red + dr);
			*green = (uint8_t)(*green + dg);
			*blue = (uint8_t)(*blue + db);
		} else {
			*run = op & 0x3FU;
		}
	}

	uint32_t slot = login_wallpaper_hash(*red, *green, *blue);
	index[slot][0] = *red;
	index[slot][1] = *green;
	index[slot][2] = *blue;
	index[slot][3] = 255U;
	*pixel = 0xFF000000U | (uint32_t)*red << 16 | (uint32_t)*green << 8 | *blue;
	return true;
}

bool
login_wallpaper_draw(display_device_t *display)
{
	const uint8_t *cursor = nxu_login_wallpaper_start;
	const uint8_t *end = nxu_login_wallpaper_end;
	if (display == 0 || display->framebuffer == 0 || display->width == 0U || display->height == 0U ||
		display->width > LOGIN_WALLPAPER_MAX_WIDTH || display->height > LOGIN_WALLPAPER_MAX_HEIGHT ||
		display->stride < display->width || (size_t)(end - cursor) < 14U || memcmp(cursor, "qoif", 4U) != 0)
		return false;

	uint32_t source_width =
		(uint32_t)cursor[4] << 24 | (uint32_t)cursor[5] << 16 | (uint32_t)cursor[6] << 8 | cursor[7];
	uint32_t source_height =
		(uint32_t)cursor[8] << 24 | (uint32_t)cursor[9] << 16 | (uint32_t)cursor[10] << 8 | cursor[11];
	if (source_width == 0U || source_width > 1920U || source_height == 0U || source_height > 1080U || cursor[12] != 3U)
		return false;

	cursor += 14U;

	uint64_t scale_x = (uint64_t)display->width * 65536U / source_width;
	uint64_t scale_y = (uint64_t)display->height * 65536U / source_height;
	uint64_t scale = scale_x > scale_y ? scale_x : scale_y;
	uint32_t scaled_width = (uint32_t)((source_width * scale + 65535U) >> 16);
	uint32_t scaled_height = (uint32_t)((source_height * scale + 65535U) >> 16);
	uint32_t crop_x = scaled_width > display->width ? (scaled_width - display->width) / 2U : 0U;
	uint32_t crop_y = scaled_height > display->height ? (scaled_height - display->height) / 2U : 0U;
	for (uint32_t x = 0U; x < display->width; x++) {
		uint64_t scaled_x = (uint64_t)(x + crop_x) * 65536U / scale;
		login_wallpaper_xmap[x] = scaled_x >= source_width ? source_width - 1U : (uint32_t)scaled_x;
	}
	for (uint32_t y = 0U; y < display->height; y++) {
		uint64_t scaled_y = (uint64_t)(y + crop_y) * 65536U / scale;
		login_wallpaper_ymap[y] = scaled_y >= source_height ? source_height - 1U : (uint32_t)scaled_y;
	}

	uint8_t index[64][4] = {{0U}};
	uint8_t red = 0U;
	uint8_t green = 0U;
	uint8_t blue = 0U;
	uint32_t run = 0U;
	uint32_t output_y = 0U;
	for (uint32_t source_y = 0U; source_y < source_height; source_y++) {
		for (uint32_t source_x = 0U; source_x < source_width; source_x++) {
			uint32_t pixel;
			if (!login_wallpaper_read_pixel(&cursor, end, &red, &green, &blue, &run, index, &pixel)) return false;

			login_wallpaper_row[source_x] = pixel;
		}
		while (output_y < display->height && login_wallpaper_ymap[output_y] == source_y) {
			uint32_t *destination = display->framebuffer + (uint64_t)output_y * display->stride;
			for (uint32_t x = 0U; x < display->width; x++)
				destination[x] = login_wallpaper_row[login_wallpaper_xmap[x]];
			output_y++;
		}
	}
	return output_y == display->height;
}
