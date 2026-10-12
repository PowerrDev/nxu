#ifndef NXU_EFI_H
#define NXU_EFI_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t status;
typedef uint64_t uintn;
typedef void *handle;
typedef uint16_t char16;

typedef struct {
	uint32_t a;
	uint16_t b;
	uint16_t c;
	uint8_t d[8];
} guid;

#define EFI_ERROR(s) ((s) >> 63)
#define EFI_BUFFER_TOO_SMALL (0x8000000000000005ULL)
#define EFI_INVALID_PARAMETER (0x8000000000000002ULL)
#define EFI_LOAD_ERROR (0x8000000000000001ULL)
#define EFI_NOT_FOUND (0x800000000000000EULL)

typedef struct {
	uint64_t signature;
	uint32_t revision;
	uint32_t size;
	uint32_t crc;
	uint32_t reserved;
} table_header;

typedef struct {
	void *reset;

	status (*output)(void *, char16 *);
} text_output;

typedef struct {
	table_header header;

	void *raise_tpl;
	void *restore_tpl;

	status (*allocate_pages)(uint32_t, uint32_t, uintn, uint64_t *);
	status (*free_pages)(uint64_t, uintn);
	status (*get_memory_map)(uintn *, void *, uintn *, uintn *, uint32_t *);
	status (*allocate_pool)(uint32_t, uintn, void **);
	status (*free_pool)(void *);

	void *create_event;
	void *set_timer;
	void *wait_for_event;
	void *signal_event;
	void *close_event;
	void *check_event;
	void *install_protocol;
	void *reinstall_protocol;
	void *uninstall_protocol;

	status (*handle_protocol)(handle, guid *, void **);

	void *reserved;
	void *register_protocol_notify;
	void *locate_handle;
	void *locate_device_path;
	void *install_configuration_table;
	void *load_image;
	void *start_image;
	void *exit;
	void *unload_image;

	status (*exit_boot_services)(handle, uintn);

	void *get_next_monotonic_count;
	void *stall;

	status (*set_watchdog_timer)(uintn, uint64_t, uintn, char16 *);

	void *connect_controller;
	void *disconnect_controller;
	void *open_protocol;
	void *close_protocol;
	void *open_protocol_information;
	void *protocols_per_handle;
	void *locate_handle_buffer;

	status (*locate_protocol)(guid *, void *, void **);
} boot_services;

typedef struct {
	guid vendor;
	void *table;
} config_table;

typedef struct {
	table_header header;

	char16 *firmware_vendor;
	uint32_t firmware_revision;
	handle console_in_handle;
	void *console_in;
	handle console_out_handle;

	text_output *console_out;

	handle stderr_handle;

	text_output *stderr_out;

	void *runtime_services;

	boot_services *boot;

	uintn table_count;

	config_table *tables;
} system_table;

typedef struct {
	uint32_t revision;
	handle parent;

	system_table *system;

	handle device;
	void *file_path;
	void *reserved;
	uint32_t options_size;
	void *options;
	void *image_base;
	uint64_t image_size;
	uint32_t code_type;
	uint32_t data_type;
	void *unload;
} loaded_image;
typedef struct file file;

struct file {
	uint64_t revision;

	status (*open)(file *, file **, char16 *, uint64_t, uint64_t);
	status (*close)(file *);

	void *remove;

	status (*read)(file *, uintn *, void *);

	void *write;
	void *get_position;
	void *set_position;

	status (*get_info)(file *, guid *, uintn *, void *);
};

typedef struct {
	uint64_t revision;

	status (*open_volume)(void *, file **);
} filesystem;

typedef struct {
	uint32_t type;
	uint32_t pad;
	uint64_t physical;
	uint64_t virtual_address;
	uint64_t pages;
	uint64_t attributes;
} memory_descriptor;

typedef struct {
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t masks[4];
	uint32_t pixels_per_line;
} mode_info;

typedef struct {
	uint32_t max_mode;
	uint32_t mode;

	mode_info *info;

	uintn info_size;
	uint64_t framebuffer;
	uintn framebuffer_size;
} gop_mode;

typedef struct {
	void *query_mode;
	void *set_mode;
	void *blt;
	gop_mode *mode;
} gop;

_Static_assert(sizeof(guid) == 16, "UEFI GUID ABI");
_Static_assert(offsetof(system_table, boot) == 96, "UEFI system table ABI");
_Static_assert(offsetof(boot_services, exit_boot_services) == 232, "UEFI boot services ABI");
_Static_assert(offsetof(loaded_image, device) == 24, "UEFI loaded image ABI");
_Static_assert(sizeof(memory_descriptor) == 40, "UEFI memory descriptor ABI");
#endif
