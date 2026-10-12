#include <stdint.h>
#include <frameworks/CoreFoundation.framework/lib/plist/plist.h>
#include <nxu/syscall.h>
#include <nxu/string.h>
#include <nxu/runtime.h>
#include <nxu/gui_host_protocol.h>
extern int nxu_math_result(void), nxu_math_initializations(void);
extern void nxu_math_crash(void);
extern int optional_function(void) __attribute__((weak));
static int app_initialized;
static int (*result_pointer)(void) = nxu_math_result;

__attribute__((constructor)) static void
initialize(void)
{
	app_initialized = nxu_math_initializations() == 1;
}

static void
output(const char *text)
{
	uint64_t size = 0;
	while (text[size])
		size++;
	register uint64_t x0 __asm__("x0") = 1, x1 __asm__("x1") = (uint64_t)text, x2 __asm__("x2") = size,
						 x8 __asm__("x8") = 3;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
}

int
main(void)
{
	void *allocation = nxu_alloc(127, 65536);
	if (!allocation || ((uintptr_t)allocation & 65535)) {
		output("dyrtest: FAILED Runtime allocation\n");
		return 5;
	}
	memset(allocation, 42, 127);
	void *resized = nxu_realloc(allocation, 255, 16);
	if (!resized) {
		output("dyrtest: FAILED Runtime realloc\n");
		return 5;
	}
	for (int i = 0; i < 127; i++)
		if (((unsigned char *)resized)[i] != 42) {
			output("dyrtest: FAILED Runtime contents\n");
			return 5;
		}
	if (!nxu_free(resized)) {
		output("dyrtest: FAILED Runtime free\n");
		return 5;
	}

	union {
		nxu_gui_host_hello_t request;
		nxu_gui_host_hello_reply_t reply;
	} host = {0};

	host.request.header = (nxu_gui_host_header_t){56, 1, 0, NXU_GUI_HOST_HELLO, 0, 0, 1};
	host.request.supported_features = host.request.required_features = NXU_GUI_HOST_FEATURE_LEASE;
	host.request.max_record_size = 4096;
	if (nxu_gui_host(&host, sizeof(host)) || !host.reply.header.session_epoch) {
		output("dyrtest: FAILED GUI HELLO\n");
		return 6;
	}
	uint64_t epoch = host.reply.header.session_epoch;
	host.request.header = (nxu_gui_host_header_t){32, 1, 0, NXU_GUI_HOST_DISPLAY_ATTACH, 0, epoch + 1, 2};
	if (nxu_gui_host(&host, sizeof(host)) != -NXU_SYS_E_INVALID_ARGUMENT) {
		output("dyrtest: FAILED GUI stale epoch\n");
		return 6;
	}
	host.request.header = (nxu_gui_host_header_t){32, 1, 0, NXU_GUI_HOST_DISPLAY_ATTACH, 0, epoch, 3};
	if (nxu_gui_host(&host, sizeof(host))) {
		output("dyrtest: FAILED GUI ATTACH\n");
		return 6;
	}
	host.request.header = (nxu_gui_host_header_t){32, 1, 0, NXU_GUI_HOST_RELEASE, 0, epoch, 4};
	if (nxu_gui_host(&host, sizeof(host)) || host.reply.header.session_epoch) {
		output("dyrtest: FAILED GUI RELEASE\n");
		return 6;
	}
	output("dyrtest: Runtime allocator and GUI host passed\n");
	const char document[] = "<plist><dict><key>Runtime</key><true/></dict></plist>";
	plist_parser_t parser;
	plist_event_t event;
	if (plist_parser_init(&parser, document, sizeof(document) - 1) != PLIST_STATUS_OK ||
		plist_parser_next(&parser, &event) != PLIST_STATUS_OK || event.type != PLIST_EVENT_DICT_BEGIN) {
		output("dyrtest: FAILED CoreFoundation parser\n");
		return 4;
	}
	output("dyrtest: CoreFoundation parser passed\n");
	char address[] = "dyrtest: function=0000000000000000\n";
	uint64_t function = (uint64_t)(uintptr_t)nxu_math_result;
	for (uint32_t i = 0; i < 16; i++)
		address[18 + i] = "0123456789abcdef"[(function >> ((15 - i) * 4)) & 15];
	output(address);
	int result = result_pointer();
	if (!app_initialized || nxu_math_initializations() != 1 || optional_function) {
		output("dyrtest: FAILED initializers or weak symbol\n");
		return 2;
	}
	char arguments[512];
	if (nxu_get_boot_args(arguments, sizeof(arguments)) >= 0 && nxu_arg_present(arguments, "-dyrld-test-crash"))
		nxu_math_crash();
	if (result == 42)
		output("dyrtest: PASS result=42 initializers=1 weak=0\n");
	else if (result == 84)
		output("dyrtest: PASS result=84 initializers=1 weak=0\n");
	else {
		output("dyrtest: FAILED symbol binding\n");
		return 3;
	}
	return 0;
}
