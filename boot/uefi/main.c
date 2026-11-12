/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/main.c
 *
 * NBL's entry point and the services its architecture back ends share: read
 * the kernel, the boot arguments and the boot chime from the boot volume,
 * place the kernel, take the display, start the chime and hand over.
 */

#include "nbl.h"

static guid loaded_guid = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static guid fs_guid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static guid file_info_guid = {0x9576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static guid gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

boot_services *bs;
system_table *st;

/* The linker wants this from any image that touches floating point (the WAV reader converts float samples). */
int _fltused;

/*
 * Routine:     nbl_copy
 * Purpose:
 *              Copy a bounded byte range without relying on a UEFI C runtime.
 */
void
nbl_copy(void *d, const void *s, uintn n)
{
	uint8_t *a = d;
	const uint8_t *b = s;

	while (n--) {
		*a++ = *b++;
	}
}

/*
 * Routine:     nbl_zero
 * Purpose:
 *              Clear a memory range used for a kernel image or boot handoff.
 */
void
nbl_zero(void *d, uintn n)
{
	uint8_t *a = d;

	while (n--) {
		*a++ = 0;
	}
}

/* The compiler may still ask for these by name when it copies or clears a structure. */
void *
memcpy(void *d, const void *s, size_t n)
{
	nbl_copy(d, s, n);
	return d;
}

void *
memset(void *d, int value, size_t n)
{
	uint8_t *a = d;

	while (n--) {
		*a++ = (uint8_t)value;
	}

	return d;
}

/*
 * Routine:     nbl_equal
 * Purpose:
 *              Compare fixed-size firmware identifiers without libc support.
 */
int
nbl_equal(const void *a, const void *b, uintn n)
{
	const uint8_t *x = a, *y = b;

	while (n--) {
		if (*x++ != *y++) {
			return 0;
		}
	}

	return 1;
}

uintn
nbl_length(const char *string)
{
	uintn length = 0;

	while (string[length]) {
		length++;
	}

	return length;
}

/*
 * Routine:     nbl_log
 * Purpose:
 *              Send one status line to the UEFI console before handoff. The
 *              console takes UTF-16; the messages are ASCII.
 */
void
nbl_log(const char *message)
{
	char16 line[160];
	uintn length = 0;

	while (message[length] && length < 156) {
		line[length] = (uint8_t)message[length];
		length++;
	}

	line[length++] = '\r';
	line[length++] = '\n';
	line[length] = 0;
	st->console_out->output(st->console_out, line);
}

void
nbl_log_hex(const char *label, uint64_t value)
{
	char line[150];
	uintn length = 0;

	while (label[length] && length < 120) {
		line[length] = label[length];
		length++;
	}

	line[length++] = '0';
	line[length++] = 'x';
	for (int shift = 60; shift >= 0; shift -= 4) {
		line[length++] = "0123456789abcdef"[(value >> shift) & 15];
	}

	line[length] = 0;
	nbl_log(line);
}

void *
nbl_config_table(const guid *vendor)
{
	for (uintn i = 0; i < st->table_count; i++) {
		if (nbl_equal(&st->tables[i].vendor, vendor, sizeof(guid))) {
			return st->tables[i].table;
		}
	}

	return 0;
}

/*
 * Routine:     nbl_read_file
 * Purpose:
 *              Read one bounded file from the EFI system partition using the
 *              filesystem protocol attached to this loader image.
 */
status
nbl_read_file(handle image, char16 *path, void **out, uintn *size)
{
	loaded_image *loaded = 0;
	filesystem *fs = 0;
	file *root = 0, *f = 0;
	status s = bs->handle_protocol(image, &loaded_guid, (void **)&loaded);

	if (EFI_ERROR(s)) {
		return s;
	}

	s = bs->handle_protocol(loaded->device, &fs_guid, (void **)&fs);
	if (EFI_ERROR(s)) {
		return s;
	}

	s = fs->open_volume(fs, &root);
	if (EFI_ERROR(s)) {
		return s;
	}

	s = root->open(root, &f, path, 1, 0);
	root->close(root);
	if (EFI_ERROR(s)) {
		return s;
	}

	uint8_t info[1024];
	uintn info_size = sizeof(info);

	s = f->get_info(f, &file_info_guid, &info_size, info);
	if (EFI_ERROR(s)) {
		f->close(f);
		return s;
	}

	uint64_t length;

	nbl_copy(&length, info + 8, 8);
	if (!length || length > 64 * 1024 * 1024) {
		f->close(f);
		return EFI_LOAD_ERROR;
	}

	*size = length;
	s = bs->allocate_pool(2, *size, out);
	if (!EFI_ERROR(s)) {
		uintn read_size = *size;

		s = f->read(f, &read_size, *out);
		if (!EFI_ERROR(s) && read_size != *size) {
			s = EFI_LOAD_ERROR;
		}

		if (EFI_ERROR(s)) {
			bs->free_pool(*out);
			*out = 0;
		}
	}

	f->close(f);
	return s;
}

bool
nbl_bootarg_present(const char *arguments, const char *word)
{
	uintn length = nbl_length(word);

	for (const char *cursor = arguments; *cursor;) {
		while (*cursor == ' ' || *cursor == '\t') {
			cursor++;
		}

		const char *start = cursor;

		while (*cursor && *cursor != ' ' && *cursor != '\t') {
			cursor++;
		}

		if ((uintn)(cursor - start) == length && nbl_equal(start, word, length)) {
			return true;
		}
	}

	return false;
}

/*
 * Routine:     read_bootargs
 * Purpose:
 *              The optional \NXU\bootargs.txt as one line of text; an empty
 *              line when the file is absent.
 */
static void
read_bootargs(handle image, char *arguments)
{
	void *text = 0;
	uintn size = 0;

	arguments[0] = 0;
	if (EFI_ERROR(nbl_read_file(image, (char16 *)u"\\NXU\\bootargs.txt", &text, &size))) {
		return;
	}

	if (size > NBL_BOOTARGS_MAX - 1) {
		size = NBL_BOOTARGS_MAX - 1;
	}

	nbl_copy(arguments, text, size);
	for (uintn i = 0; i < size; i++) {
		if (arguments[i] == '\n' || arguments[i] == '\r') {
			arguments[i] = ' ';
		}
	}

	/* The image builder ends the file with a newline; the kernel wants no trailing blank. */
	while (size && arguments[size - 1] == ' ') {
		size--;
	}

	arguments[size] = 0;
	bs->free_pool(text);
}

/*
 * Routine:     display_start
 * Purpose:
 *              Take over the display the firmware initialized: true when
 *              there is one with an active mode. A linear 32-bit framebuffer
 *              is cleared to black, so the screen is the loader's from the
 *              moment the chime sounds. The chime is only played with a
 *              display up; without one it is left to the kernel, which plays
 *              it when its own display output starts.
 */
static bool
display_start(void)
{
	gop *graphics = 0;

	if (EFI_ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&graphics)) || !graphics) {
		/*
		 * A firmware that boots straight from its boot order connects only
		 * the boot device. Have it bind its drivers to every controller,
		 * the display's among them, and look again.
		 */
		handle *handles = 0;
		uintn count = 0;

		if (!EFI_ERROR(bs->locate_handle_buffer(0, 0, 0, &count, &handles))) {
			for (uintn i = 0; i < count; i++) {
				(void)bs->connect_controller(handles[i], 0, 0, 1);
			}

			bs->free_pool(handles);
		}

		graphics = 0;
		if (EFI_ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&graphics))) {
			return false;
		}
	}

	if (!graphics || !graphics->mode || !graphics->mode->info || !graphics->mode->info->width ||
		!graphics->mode->info->height) {
		return false;
	}

	gop_mode *mode = graphics->mode;

	/* Formats 0 and 1 are RGB and BGR in 32 bits; anything else has no framebuffer to write. */
	if (mode->info->format < 2 && mode->framebuffer) {
		volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)mode->framebuffer;
		uintn count = (uintn)mode->info->pixels_per_line * mode->info->height;

		if (count > mode->framebuffer_size / 4) {
			count = mode->framebuffer_size / 4;
		}

		for (uintn i = 0; i < count; i++) {
			pixels[i] = 0;
		}
	}

	return true;
}

/*
 * Routine:     announce_chime
 * Purpose:
 *              Tell the kernel how long the chime the loader started runs,
 *              as the first boot argument, where a kernel that keeps only
 *              the head of a long line still finds it.
 */
static void
announce_chime(char *arguments, uint32_t milliseconds)
{
	char word[40];
	uintn length = 0;

	for (const char *name = NBL_CHIME_BOOTARG; *name; name++) {
		word[length++] = *name;
	}

	word[length++] = '=';

	char digits[10];
	uintn count = 0;

	do {
		digits[count++] = (char)('0' + milliseconds % 10);
		milliseconds /= 10;
	} while (milliseconds);

	while (count) {
		word[length++] = digits[--count];
	}

	uintn used = nbl_length(arguments);

	if (used) {
		word[length++] = ' ';
	}

	/* Too long a line loses its tail rather than the announcement. */
	if (used + length > NBL_BOOTARGS_MAX - 1) {
		used = NBL_BOOTARGS_MAX - 1 - length;
	}

	for (uintn i = used; i > 0; i--) {
		arguments[i - 1 + length] = arguments[i - 1];
	}

	nbl_copy(arguments, word, length);
	arguments[used + length] = 0;
}

/*
 * Routine:     efi_main
 * Purpose:
 *              Load the NXU kernel for this firmware's architecture, start
 *              the boot chime and transfer control to the kernel.
 */
status
efi_main(handle image, system_table *system)
{
	static char arguments[NBL_BOOTARGS_MAX];

	st = system;
	bs = system->boot;
	(void)bs->set_watchdog_timer(0, 0, 0, 0);
	nbl_log("efi_main: NBL preparing kernel handoff");

	void *kernel = 0;
	uintn kernel_size = 0;
	status s = nbl_read_file(image, (char16 *)u"\\NXU\\kernel.elf", &kernel, &kernel_size);

	if (EFI_ERROR(s)) {
		nbl_log("efi_main: cannot read kernel image");
		return s;
	}

	/* The kernel's fixed load range is claimed first, so nothing allocated below can land in it. */
	s = nbl_arch_load(kernel, kernel_size);
	bs->free_pool(kernel);
	if (EFI_ERROR(s)) {
		nbl_log("efi_main: kernel image is invalid or cannot be loaded");
		return s;
	}

	read_bootargs(image, arguments);

	if (nbl_bootarg_present(arguments, "-no-chime") || nbl_bootarg_present(arguments, "-no-sound")) {
		nbl_log("efi_main: boot chime disabled by the boot arguments");
	} else if (!display_start()) {
		nbl_log("efi_main: no display: the boot chime is left to the kernel");
	} else {
		nbl_log("efi_main: display initialized");

		uint32_t milliseconds = nbl_chime_play(image);

		if (milliseconds) {
			announce_chime(arguments, milliseconds);
		}
	}

	s = nbl_arch_enter(image, arguments);
	nbl_log("efi_main: kernel handoff failed");
	return s;
}
