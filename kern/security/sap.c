/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/security/sap.c
 *
 * Implements secure anchor policy and authorization checks.
 */
#include <kern/security/sap.h>

#include <kern/boot/boot_mode.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/process/signal.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/machine/timer.h>
#include <kern/memory/heap.h>
#include <kern/lock.h>
#include <libk/string.h>
#include <kern/console/console.h>

#if defined(__aarch64__) || defined(__i386__)

#include <drivers/video/ui_service_host.h>
#include <drivers/video/display.h>
#include <frameworks/include/nxu/pointer_cursor.h>
#include <drivers/input/keyboard.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/tep/tep_crypto.h>
#include <reporter_fonts.h>

#endif

static nxu_spinlock_t g_lock;
static waitq_t g_wait;
static bool g_worker_started;

static struct {
	uint32_t state; /* 0 free, 1 pending, 2 running, 3 completed */
	proc_ident_t owner;
	char path[VFS_PATH_MAX];
	bool cancelled;
	bool recursive;
	vfs_status_t result;
} g_request;

bool
sap_canonical_path(const char *path, char *out)
{
	if (path == 0 || path[0] != '/') return false;

	uint32_t length = 0, cursor = 1;
	out[length++] = '/';

	while (path[cursor]) {
		while (path[cursor] == '/') {
			cursor++;
		}

		uint32_t first = cursor;
		while (path[cursor] && path[cursor] != '/') {
			cursor++;
		}

		uint32_t size = cursor - first;
		if (size == 0) break;

		if (size == 1 && path[first] == '.') continue;

		/* Refuse parent traversal rather than guessing across mount roots. */
		if (size == 2 && path[first] == '.' && path[first + 1] == '.') return false;

		if (size > VFS_NAME_MAX || length + size + 1 >= VFS_PATH_MAX) return false;

		if (length > 1) {
			out[length++] = '/';
		}

		memcpy(out + length, path + first, size);
		length += size;
	}

	out[length] = 0;
	return length > 1;
}

static bool
below(const char *path, const char *root)
{
	size_t n = strlen(root);

	return strlen(path) >= n && memcmp(path, root, n) == 0 && (path[n] == 0 || path[n] == '/');
}

bool
sap_protected_path(const char *path)
{
	return below(path, "/Volumes/mainOS/System") || below(path, "/Volumes/mainOS/Library");
}

static bool
cancelled(void)
{
	return __atomic_load_n(&g_request.cancelled, __ATOMIC_ACQUIRE);
}

#if defined(__aarch64__) || defined(__i386__)
static void
wipe(void *memory, uint32_t length)
{
	volatile uint8_t *p = memory;

	while (length--) {
		*p++ = 0;
	}
}

static void
text(display_device_t *display, int32_t x, int32_t y, const char *string, const reporter_font_t *font, int32_t right,
	 int32_t bottom)
{
	int32_t start = x;

	while (*string && y + font->height < bottom) {
		uint32_t code = (uint8_t)*string++;
		const reporter_glyph_t *g = &font->glyphs[(code >= 32 && code <= 126 ? code : '?') - 32];

		if (code == '\n' || x + g->advance > right) {
			x = start;
			y += font->height + 3;

			if (code == '\n') continue;
		}

		if (y + font->height >= bottom) break;

		for (uint32_t row = 0; row < g->height; row++) {
			for (uint32_t col = 0; col < g->width; col++) {
				int32_t px = x + g->x + col, py = y + g->y + row;

				if (px < 0 || py < 0 || px >= (int32_t)display->width || py >= bottom) continue;

				uint32_t alpha = font->pixels[g->offset + row * g->width + col];
				uint32_t old = display->framebuffer[py * display->stride + px], color = 0xff000000;

				for (uint32_t shift = 0; shift < 24; shift += 8) {
					color |= (((old >> shift & 255) * (255 - alpha) + 32 * alpha + 127) / 255) << shift;
				}

				display->framebuffer[py * display->stride + px] = color;
			}
		}

		x += g->advance;
	}
}

static uint32_t
blend(uint32_t under, uint32_t over, uint32_t alpha)
{
	uint32_t inverse = 255U - alpha, result = 0xff000000U;
	for (uint32_t shift = 0; shift < 24; shift += 8) {
		uint32_t a = (under >> shift) & 255U, b = (over >> shift) & 255U;
		result |= ((a * inverse + b * alpha + 127U) / 255U) << shift;
	}
	return result;
}

static void
blurred_panel(display_device_t *d, const uint32_t *source, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		  uint32_t radius)
{
	for (uint32_t row = 0; row < h && y + row < d->height; row++) {
		for (uint32_t col = 0; col < w && x + col < d->width; col++) {
			uint32_t cx = col < radius ? radius - col : (col >= w - radius ? col - (w - radius - 1) : 0);
			uint32_t cy = row < radius ? radius - row : (row >= h - radius ? row - (h - radius - 1) : 0);
			
			if (cx && cy && cx * cx + cy * cy > radius * radius) continue;
			uint32_t sum[3] = {0, 0, 0}, samples = 0;
			
			for (int32_t oy = -3; oy <= 3; oy++) {
				int32_t py = (int32_t)(y + row) + oy;
				if (py < 0 || py >= (int32_t)d->height) continue;
			
				for (int32_t ox = -3; ox <= 3; ox++) {
					int32_t px = (int32_t)(x + col) + ox;
					if (px < 0 || px >= (int32_t)d->width) continue;
					uint32_t pixel = source[(uint32_t)py * d->stride + (uint32_t)px];
					for (uint32_t channel = 0; channel < 3; channel++) sum[channel] += (pixel >> (channel * 8)) & 255U;
					samples++;
				}
			}
			
			uint32_t color = 0xff000000U;
			
			for (uint32_t channel = 0; channel < 3; channel++) color |= ((sum[channel] / samples) & 255U) << (channel * 8);
			
			uint32_t *pixel = &d->framebuffer[(y + row) * d->stride + x + col];
			*pixel = blend(*pixel, color, 184);
		}
	}
}

static void
rounded(display_device_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color,
		uint32_t alpha)
{
	if (radius > w / 2) radius = w / 2;
	if (radius > h / 2) radius = h / 2;
	
	for (uint32_t row = 0; row < h && y + row < d->height; row++) {
		for (uint32_t col = 0; col < w && x + col < d->width; col++) {
			uint32_t coverage = 0;
	
			for (uint32_t sy = 0; sy < 4; sy++) {
				for (uint32_t sx = 0; sx < 4; sx++) {
					int32_t px = (int32_t)(col * 4 + sx) - (int32_t)(radius * 4);
					int32_t py = (int32_t)(row * 4 + sy) - (int32_t)(radius * 4);
					int32_t max_x = (int32_t)((w - radius * 2) * 4);
					int32_t max_y = (int32_t)((h - radius * 2) * 4);
					
					if (px > max_x) px = max_x;
					if (py > max_y) py = max_y;
					if (px < 0) px = 0;
					if (py < 0) py = 0;
					
					int32_t dx = (int32_t)(col * 4 + sx) - ((int32_t)(radius * 4) + px);
					int32_t dy = (int32_t)(row * 4 + sy) - ((int32_t)(radius * 4) + py);
				
					if (dx * dx + dy * dy <= (int32_t)(radius * radius * 16)) coverage++;
				}
			}

			if (coverage == 0) continue;
			
			uint32_t *pixel = &d->framebuffer[(y + row) * d->stride + x + col];
			*pixel = blend(*pixel, color, (alpha * coverage + 8) / 16);
		}
	}
}

static bool
prompt(const char *path, bool recursive)
{
	display_device_t *d = display_primary();

	if (d == 0 || d->framebuffer == 0 || d->width < 600 || d->height < 360 || d->stride < d->width ||
		d->stride > 8192 || d->height > 8192 || !ui_service_secure_begin()) {
		return false;
	}

	uint64_t bytes = (uint64_t)d->stride * d->height * 4;
	uint32_t *saved = kmalloc(bytes);

	if (saved == 0) {
		ui_service_secure_end();
		return false;
	}

	memcpy(saved, d->framebuffer, bytes);
	kputln("sap: confirmation requested");

	keyboard_set_secure(true);

	keyboard_flush_keys();
	input_flush();

	uint32_t x = (d->width - 560) / 2, y = (d->height - 300) / 2;
	uint32_t stage = 0, length = 0;
	char password[65] = {0};
	const char *message = "";
	bool approved = false;

	int32_t pointer_x = 0, pointer_y = 0;
	uint32_t pointer_down = 0;
	(void)ui_service_secure_pointer(&pointer_x, &pointer_y, &pointer_down);

	uint64_t expiry = timer_get_microseconds() + 120000000ULL;
	uint64_t animation_start = timer_get_microseconds();

	for (;;) {
		for (uint64_t i = 0; i < bytes / 4; i++) {
			d->framebuffer[i] = (saved[i] & 0xff000000) | ((saved[i] & 0xfefefe) >> 1);
		}

		/* Expand the panel from its center on first presentation. */
		uint64_t elapsed = timer_get_microseconds() - animation_start;
		uint32_t progress = elapsed >= 140000ULL ? 256U : (uint32_t)(elapsed * 256U / 140000ULL);
		uint32_t scale = 230U + (26U * progress / 256U);
		uint32_t panel_w = 560U * scale / 256U, panel_h = 300U * scale / 256U;
		uint32_t panel_x = x + (560U - panel_w) / 2U, panel_y = y + (300U - panel_h) / 2U;
		
		/* A bright translucent panel over the dimmed desktop gives the prompt
		 * the soft, frosted surface without compromising trusted scanout. */
		blurred_panel(d, saved, panel_x, panel_y, panel_w, panel_h, 24U * scale / 256U);
		
		rounded(d, panel_x, panel_y, panel_w, panel_h, 24U * scale / 256U, 0xfff5f8ff, 112);
		rounded(d, panel_x + 1, panel_y + 1, panel_w - 2, 1, 1, 0xffffffff, 210);
		
		if (progress == 256U) {
		
		text(d, x + 28, y + 18, "Secure Anchor Protocol", &semibold, x + 532, y + 52);
		text(d, x + 28, y + 56,
			 recursive ? "Delete this folder and everything inside it? This could break\nyour system."
					   : "Deleting this file could break your entire system and make it\nunusable. Are you sure?",
			 &regular, x + 532, y + 112);
		
		text(d, x + 28, y + 112, path + 5, &regular, x + 532, y + 171); /* Hide the VFS mount prefix. */

		if (stage) {
			text(d, x + 28, y + 174, "Password:", &regular, x + 110, y + 208);
			rounded(d, x + 116, y + 173, 416, 32, 9, 0xffdce8f8, 180);
			rounded(d, x + 118, y + 175, 412, 28, 8, 0xfffbfdff, 220);

			char masked[65];
			memset(masked, '*', length);
			masked[length] = 0;

			text(d, x + 124, y + 177, masked, &regular, x + 524, y + 208);
			wipe(masked, sizeof(masked));
		}

		text(d, x + 28, y + 211, message, &regular, x + 532, y + 244);

		rounded(d, x + 316, y + 252, 96, 34, 12, 0xffe2e8f1, 220);
		rounded(d, x + 428, y + 252, 104, 34, 12, 0xff0878f9, 255);

		text(d, x + 338, y + 258, "Cancel", &regular, x + 408, y + 290);
		text(d, x + 443, y + 258, stage ? "Delete" : "Continue", &regular, x + 530, y + 290);
		}

		/* SAP owns input and scanout, but uses mainOS's shared arrow. */
		for (uint32_t row = 0; row < NXU_POINTER_CURSOR_HEIGHT; row++) {
			for (uint32_t col = 0; col < NXU_POINTER_CURSOR_WIDTH; col++) {
				int32_t px = pointer_x - (int32_t)NXU_POINTER_CURSOR_HOTSPOT_X + (int32_t)col;
				int32_t py = pointer_y - (int32_t)NXU_POINTER_CURSOR_HOTSPOT_Y + (int32_t)row;

				if (px < 0 || py < 0 || px >= (int32_t)d->width || py >= (int32_t)d->height) {
					continue;
				}

				uint32_t *pixel = &d->framebuffer[(uint32_t)py * d->stride + (uint32_t)px];
				*pixel =
					nxu_pointer_cursor_blend(*pixel, nxu_pointer_cursor_pixels[row * NXU_POINTER_CURSOR_WIDTH + col]);
			}
		}
		if (!display_present_full(d)) break;

		bool activate = false, redraw = false;

		while (!redraw && !activate && !cancelled() && timer_get_microseconds() < expiry) {
			virtio_input_service();

			int32_t old_x = pointer_x, old_y = pointer_y;

			if (ui_service_secure_pointer(&pointer_x, &pointer_y, &pointer_down)) {
				if ((pointer_down & 1) && pointer_y >= (int32_t)y + 252 && pointer_y < (int32_t)y + 286) {
					if (pointer_x >= (int32_t)x + 316 && pointer_x < (int32_t)x + 412) {
						goto done;
					}

					if (pointer_x >= (int32_t)x + 428 && pointer_x < (int32_t)x + 532) {
						activate = true;
					}
				}

				if (old_x != pointer_x || old_y != pointer_y) {
					redraw = true;
				}
			}

			keyboard_key_t key;
			if (keyboard_take_key(&key)) {
				if (key.code == 1) {
					goto done; /* Escape */
				}

				if (key.code == 28) {
					activate = true;
				} else if (stage && key.code == 14 && length) {
					password[--length] = 0;
					redraw = true;
				} else if (stage && key.character >= 32 && key.character < 127 && length < 64) {
					password[length++] = key.character;
					password[length] = 0;
					redraw = true;
				}

				wipe(&key, sizeof(key));
			}
			if (timer_get_microseconds() - animation_start < 140000ULL) redraw = true;

			(void)sched_sleep_us(10000);
		}

		if (cancelled() || timer_get_microseconds() >= expiry) break;

		if (activate && !stage) {
			stage = 1;

			keyboard_flush_keys();
			kputln("sap: password requested");
			
			continue;
		}

		if (activate && stage) {
			uint32_t wait = 0;
			tep_result_t status = tep_auth_verify(password, length, &wait);

			wipe(password, sizeof(password));
			length = 0;

			if (status == TEP_OK) {
				approved = !cancelled();
				break;
			}

			kputln("sap: password refused");
			message = status == TEP_ERR_DENIED		  ? "Incorrect password. Try again or press Escape."
					  : status == TEP_ERR_RETRY_LATER ? "Please wait before trying again, or press Escape."
													  : "Password verification is unavailable. Press Escape to cancel.";
		}
	}

done:
	wipe(password, sizeof(password));

	keyboard_flush_keys();
	input_flush();

	memcpy(d->framebuffer, saved, bytes);
	(void)display_present_full(d);
	kfree(saved);

	keyboard_flush_keys();
	input_flush();
	keyboard_set_secure(false);
	ui_service_secure_end();

	return approved;
}

#else
static bool
prompt(const char *path, bool recursive)
{
	(void)path;
	(void)recursive;
	return false;
}

#endif

static void
sap_worker(void *unused)
{
	(void)unused;

	for (;;) {
		uint32_t sequence = waitq_seq(&g_wait);

		uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
		bool pending = g_request.state == 1;

		if (pending) {
			g_request.state = 2;
		}

		nxu_spin_unlock_irqrestore(&g_lock, irq);
		
		if (!pending) {
			(void)waitq_block_seq(&g_wait, sequence, true);
			continue;
		}

		vnode_t target = 0;
		vfs_status_t result = vfs_lookup(g_request.path, &target);

		if (result == VFS_STATUS_OK &&
			target->v_type != (g_request.recursive ? VNODE_TYPE_DIRECTORY : VNODE_TYPE_REGULAR)) {
			result = g_request.recursive ? VFS_STATUS_NOT_DIRECTORY : VFS_STATUS_IS_DIRECTORY;
		}
		if (result == VFS_STATUS_OK && g_request.recursive &&
			(target->v_mount == 0 || target == target->v_mount->m_root)) {
			result = VFS_STATUS_BUSY;
		}
		if (result == VFS_STATUS_OK && !cancelled() && prompt(g_request.path, g_request.recursive) && !cancelled()) {
			result = g_request.recursive ? vfs_remove_tree_held(g_request.path, &target, cancelled)
										 : vfs_unlink_held(g_request.path, &target);

			if (result == VFS_STATUS_OK) {
				result = vfs_sync_all();
			}
		} else if (result == VFS_STATUS_OK) {
			result = VFS_STATUS_DENIED;
		}

		if (target != 0) {
			vnode_rele(target);
		}

		irq = nxu_spin_lock_irqsave(&g_lock);
		g_request.result = result;
		g_request.state = cancelled() ? 0 : 3;
		nxu_spin_unlock_irqrestore(&g_lock, irq);

		waitq_wake_all(&g_wait);
	}
}

static vfs_status_t
sap_submit(proc_t caller, const char *path, bool recursive)
{
	char canonical[VFS_PATH_MAX] = {0};
	if (!sap_canonical_path(path, canonical) || !below(canonical, "/Volumes/mainOS")) {
		return VFS_STATUS_INVALID;
	}

	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	if (g_request.state != 0) {
		nxu_spin_unlock_irqrestore(&g_lock, irq);
		return VFS_STATUS_BUSY;
	}

	bool start = !g_worker_started;

	g_worker_started = true;
	g_request.owner = caller->p_ident;
	g_request.recursive = recursive;

	memcpy(g_request.path, canonical, sizeof(canonical));
	__atomic_store_n(&g_request.cancelled, false, __ATOMIC_RELEASE);
	
	g_request.state = 1;
	
	nxu_spin_unlock_irqrestore(&g_lock, irq);

	if (start) {
		thread_t thread = 0;
		bool created = kernel_thread_create(proc_task(proc_kernel()), sap_worker, 0, &thread);
		bool started = created && sched_thread_start(thread);

		if (created && !started) {
			(void)thread_terminate(thread);
			(void)thread_reap(thread);
		}

		if (thread != 0) {
			thread_deallocate(thread);
		}

		if (!started) {
			irq = nxu_spin_lock_irqsave(&g_lock);
			g_worker_started = false;
			g_request.state = 0;

			nxu_spin_unlock_irqrestore(&g_lock, irq);
			
			return VFS_STATUS_NO_MEMORY;
		}

	}

	waitq_wake_all(&g_wait);

	for (;;) {
		uint32_t sequence = waitq_seq(&g_wait);
		irq = nxu_spin_lock_irqsave(&g_lock);

		if (g_request.state == 3) {
			vfs_status_t result = g_request.result;
			g_request.state = 0;
			nxu_spin_unlock_irqrestore(&g_lock, irq);
			return result;
		}

		nxu_spin_unlock_irqrestore(&g_lock, irq);

		if (signal_pending_for(caller, current_thread()) || !waitq_block_seq(&g_wait, sequence, true)) {
			sap_cancel(caller);
		
			return VFS_STATUS_INTERRUPTED;
		}

	}
}

void
sap_cancel(proc_t caller)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_lock);
	
	if (g_request.state && g_request.owner.uniqueid == caller->p_ident.uniqueid) {
		__atomic_store_n(&g_request.cancelled, true, __ATOMIC_RELEASE);

		if (g_request.state == 3) {
			g_request.state = 0;
		}
	}

	nxu_spin_unlock_irqrestore(&g_lock, irq);

	waitq_wake_all(&g_wait);
}

vfs_status_t
sap_delete(proc_t caller, const char *path)
{
	return sap_submit(caller, path, false);
}

vfs_status_t
sap_remove_tree(proc_t caller, const char *path)
{
	if (caller == 0 || !boot_mode_is_triage_os() || caller->p_ident.pid != 1U ||
		!proc_has_caps(caller, NXU_CAP_FS_WRITE)) {
		return VFS_STATUS_DENIED;
	}

	return sap_submit(caller, path, true);
}
