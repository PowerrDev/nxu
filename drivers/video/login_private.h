/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/login_private.h
 *
 * What the kernel login screen (keystore_manager.c) shares with the
 * first-boot setup assistant (setup_assistant.c): its text faces, colour
 * blending, file reading, and the pointer it draws itself.
 */
#ifndef NXU_DRIVERS_VIDEO_LOGIN_PRIVATE_H
#define NXU_DRIVERS_VIDEO_LOGIN_PRIVATE_H

#include <drivers/video/display.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The text faces, all SF Pro (see tools/generate_login_font.py). */
typedef enum {
	LOGIN_FACE_REGULAR,	 /* 15 px */
	LOGIN_FACE_SEMIBOLD, /* 16 px */
	LOGIN_FACE_TITLE,	 /* 30 px */
	LOGIN_FACE_HEADING,	 /* 22 px */
	LOGIN_FACE_SMALL,	 /* 13 px */
	LOGIN_FACE_SMALL_BOLD, /* 13 px, semibold */
} login_face_t;

/* background towards foreground by alpha (of 255), per channel, XRGB. */
uint32_t login_blend(uint32_t background, uint32_t foreground, uint32_t alpha);

/* Text in a face, each glyph pixel `scale` times over. The colour is ARGB: its alpha fades the text. */
uint32_t login_face_width(login_face_t face, const char *text, uint32_t scale);
uint32_t login_face_height(login_face_t face, uint32_t scale);
void login_face_draw(display_device_t *display, int32_t x, int32_t y, const char *text, login_face_t face,
					 uint32_t scale, uint32_t argb);
/* The same, drawing only inside [left, right) x [top, bottom): for repainting part of a screen. */
void login_face_draw_clipped(display_device_t *display, int32_t x, int32_t y, const char *text, login_face_t face,
							 uint32_t scale, uint32_t argb, int32_t left, int32_t top, int32_t right, int32_t bottom);

/* A whole file from the VFS into a fresh kmalloc buffer, if it is at most limit bytes. */
uint8_t *login_read_file(const char *path, size_t *size, uint64_t limit);

/* The <string> value of key in a flat property list. */
bool login_plist_string(const char *text, size_t length, const char *key, char *value, size_t capacity);

/*
 * The pointer. login_pointer_poll reads the pointer, moves the cursor if it
 * moved (presenting it), and says where it is and which buttons are down.
 * Painting over the cursor: hide it first, show it again after.
 */
bool login_pointer_poll(display_device_t *display, int32_t *x, int32_t *y, uint32_t *pressed);
void login_cursor_hide(display_device_t *display);
void login_cursor_show_again(display_device_t *display);
void login_cursor_present(display_device_t *display);

#endif
