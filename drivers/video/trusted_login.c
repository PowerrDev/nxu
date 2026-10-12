/* Kernel-owned authentication UI for the userland desktop. */
#include <drivers/video/ui_service_host.h>
#include <drivers/video/display.h>
#include <drivers/video/ramfb_console.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/tep/tep_crypto.h>
#include <drivers/tep/tep_mailbox.h>
#include <kern/console/console.h>
#include <kern/process/thread.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <libk/string.h>
#include <reporter_fonts.h>

static void wipe(void *memory, size_t size)
{
	volatile uint8_t *bytes = memory;

	while (size--)
		*bytes++ = 0;
}

static void label(display_device_t *display, uint32_t x, uint32_t y, const char *text)
{
	while (*text) {
		uint32_t code = (uint8_t)*text++;
		const reporter_glyph_t *glyph = &regular.glyphs[(code >= 32 && code <= 126 ? code : '?') - 32];

		for (uint32_t row = 0; row < glyph->height; row++) {
			for (uint32_t column = 0; column < glyph->width; column++) {
				int32_t px = x + glyph->x + column;
				int32_t py = y + glyph->y + row;

				if (px < 0 || py < 0 || px >= (int32_t)display->width || py >= (int32_t)display->height)
					continue;

				uint32_t alpha = regular.pixels[glyph->offset + row * glyph->width + column];
				uint32_t old = display->framebuffer[py * display->stride + px];
				uint32_t color = 0xff000000;

				for (uint32_t shift = 0; shift < 24; shift += 8)
					color |= (((old >> shift & 255) * (255 - alpha) + 32 * alpha + 127) / 255) << shift;

				display->framebuffer[py * display->stride + px] = color;
			}
		}

		x += glyph->advance;
	}
}

static void login_thread(void *unused)
{
	(void)unused;

	display_device_t *display = display_primary();

	/* A missing renderer never grants access to the ordinary desktop. */
	if (!display || !display->framebuffer || display->width < 600 || display->height < 360 ||
		display->stride < display->width)
		return;

#if defined(__aarch64__)
	(void)ramfb_console_set_mirroring(false);
#endif
	keyboard_set_secure(true);
	keyboard_flush_keys();
	input_flush();
	kputln("trusted-login: waiting for tepOS authentication");

	uint8_t password[65] = {0}, first[65] = {0};
	uint32_t length = 0, first_length = 0;
	bool setting = false, confirming = false;
	const char *message = "";

	for (;;) {
		tep_auth_status_t status = {0};
		tep_result_t available = tep_auth_status(&status);

		if (available == TEP_OK)
			setting = !status.passcode_set;

		uint32_t x = (display->width - 560) / 2;
		uint32_t y = (display->height - 260) / 2;

		for (uint32_t row = 0; row < display->height; row++) {
			for (uint32_t column = 0; column < display->width; column++) {
				bool panel = column >= x && column < x + 560 && row >= y && row < y + 260;
				display->framebuffer[row * display->stride + column] = panel ? 0xffefeff1 : 0xff292a35;
			}
		}

		label(display, x + 28, y + 24, "Welcome to sevOS");
		label(display, x + 28, y + 65,
			  available != TEP_OK ? "Waiting for the Trusted Enclave..."
			  : confirming		  ? "Confirm your new password:"
			  : setting			  ? "Create your password:"
								  : "Enter your password:");

		char masked[65];
		memset(masked, '*', length);
		masked[length] = 0;
		label(display, x + 28, y + 105, masked);
		wipe(masked, sizeof(masked));
		label(display, x + 28, y + 150, message);
		label(display, x + 28, y + 206, "Press Return to continue");
		(void)display_present_full(display);

		bool changed = false;

		for (uint32_t poll = 0; poll < 50 && !changed; poll++) {
			virtio_input_service();
			keyboard_key_t key;

			if (keyboard_take_key(&key)) {
				changed = true;

				if (key.code == 14 && length)
					password[--length] = 0;
				else if (key.code == 1) {
					wipe(password, sizeof(password));
					wipe(first, sizeof(first));
					length = first_length = 0;
					confirming = false;
				} else if (key.code == 28 && available == TEP_OK) {
					uint32_t wait_seconds = 0;
					tep_result_t result;

					if (setting && !confirming) {
						if (length < 4) {
							message = "Use at least four characters.";
							wipe(&key, sizeof(key));
							continue;
						}

						memcpy(first, password, length);
						first_length = length;
						confirming = true;
						wipe(password, sizeof(password));
						length = 0;
						wipe(&key, sizeof(key));
						continue;
					}

					if (setting && (length != first_length || memcmp(first, password, length)))
						result = TEP_ERR_INVALID;
					else if (setting)
						result = tep_auth_set(0, 0, password, length, &wait_seconds);
					else
						result = tep_auth_verify(password, length, &wait_seconds);

					wipe(password, sizeof(password));
					wipe(first, sizeof(first));
					length = first_length = 0;
					confirming = false;

					if (result == TEP_OK) {
						wipe(&key, sizeof(key));
						keyboard_flush_keys();
						input_flush();
						keyboard_set_secure(false);
						ui_service_secure_end();
						kputln("trusted-login: unlocked by tepOS");
						return;
					}

					kputln("trusted-login: password refused");
					message = result == TEP_ERR_INVALID	 ? "Passwords did not match. Try again."
							  : result == TEP_ERR_DENIED ? "Incorrect password. Try again."
														 : "Authentication unavailable or delayed. Try again.";
				} else if (key.character >= 32 && key.character < 127 && length < 64) {
					password[length++] = key.character;
					password[length] = 0;
				}

				wipe(&key, sizeof(key));
			}

			(void)sched_sleep_us(10000);
		}
	}
}

bool ui_service_trusted_prepare(void)
{
	if (tep_mailbox_link_state() == TEP_LINK_ABSENT) {
		kputln("trusted-login: no enclave link");
		kputln("trusted-login: no login screen");
		return true;
	}

	/* Installed before PID 1 can execute. Neither display claims nor raw input
	 * are available until the enclave accepts the kernel-owned login. */
	if (!ui_service_secure_lock())
		return false;

	thread_t thread;
	return kernel_thread_create(proc_task(proc_kernel()), login_thread, 0, &thread) && sched_thread_start(thread);
}
