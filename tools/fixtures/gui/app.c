#include <WindowServer/WSPrivate.h>
#include <nxu/syscall.h>
#include <nxu/runtime.h>
#include <nxu/string.h>
extern bool nxu_rust_allocation_probe(void);
extern void nxu_rust_panic_probe(void);
int main(void);
void *const nxu_relro_anchor = (void *)(uintptr_t)main;
static unsigned presents;

static bool present(void *context, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	(void)context;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	presents++;
	return true;
}

static int fail(void)
{
	nxu_write(1, "gui-runtime: FAILED\n", 20);
	return 1;
}

int main(void)
{
	if (nxu_dyrld(NXU_DYRLD_FINALIZE, 0))
		return fail();
	nxu_dyrld_image_t image = {0};
	if (nxu_dyrld(NXU_DYRLD_LOAD, &image) != -NXU_SYS_E_BUSY)
		return fail();
	if (!nxu_rust_allocation_probe())
		return fail();
	uint32_t *frame = nxu_alloc(64 * 64 * 4, 16);
	if (!frame)
		return fail();
	memset(frame, 0, 64 * 64 * 4);
	WSDisplay display = {frame, 64, 64, 64, 0, present};
	if (!WS_Initialize(display))
		return fail();
	uint32_t pixels[16 * 16];
	for (unsigned i = 0; i < 16 * 16; i++)
		pixels[i] = 0xff336699;
	for (unsigned i = 0; i < 100; i++) {
		WSWindowID window = WS_Create_Window(2, 2, 16, 16, true, 0);
		if (!window || !WS_Render_Window(window, pixels, 16, 16, 16) || !WS_Present() || !WS_Destroy_Window(window))
			return fail();
	}
	if (!presents)
		return fail();
	nxu_write(1, "gui-runtime: PASS Rust allocation and WindowServer churn\n",
			  nxu_strlen("gui-runtime: PASS Rust allocation and WindowServer churn\n"));
	char arguments[512] = {0};
	if (nxu_get_boot_args(arguments, sizeof(arguments)) >= 0 && nxu_arg_present(arguments, "-gui-relro-fork-probe")) {
		int64_t child = nxu_fork();
		if (child < 0)
			return fail();
		if (!child) {
			*(void *volatile *)(uintptr_t)&nxu_relro_anchor = 0;
			return fail();
		}
		uint64_t status = 0;
		int64_t waited;
		do {
			waited = nxu_wait((uint64_t)child, &status);
		} while (waited == -NXU_SYS_E_INTERRUPTED);
		if (waited != child || status != NXU_EXIT_KILLED_SIGNAL(NXU_SIGSEGV))
			return fail();
		nxu_write(1, "gui-runtime: fork RELRO remained sealed\n",
				  nxu_strlen("gui-runtime: fork RELRO remained sealed\n"));
		return 0;
	}
	if (nxu_get_boot_args(arguments, sizeof(arguments)) >= 0 && nxu_arg_present(arguments, "-gui-relro-probe")) {
		*(void *volatile *)(uintptr_t)&nxu_relro_anchor = 0;
		return fail();
	}
	if (nxu_arg_present(arguments, "-gui-text-probe")) {
		*(volatile uint32_t *)(uintptr_t)WS_Present = 0;
		return fail();
	}
	if (nxu_get_boot_args(arguments, sizeof(arguments)) >= 0 && nxu_arg_present(arguments, "-gui-panic-probe")) {
		nxu_signal(NXU_SIGABRT, (void (*)(int))(uintptr_t)NXU_SIG_IGN);
		nxu_sigprocmask(NXU_SIG_BLOCK, 1U << NXU_SIGABRT);
		nxu_rust_panic_probe();
		return fail();
	}
	return 0;
}
