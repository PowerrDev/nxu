#include <kern/boot/boot_args.h>

#include <kern/boot/nvram.h>
#include <kern/console/console.h>

#include <string.h>

static char g_boot_args[BOOT_ARGS_MAX + 1U];
static bool g_boot_args_initialized;

static bool
boot_arg_separator(char character)
{
	return character == ' ' || character == '\t';
}

static uint32_t
boot_arg_string_length(const char *string)
{
	uint32_t length = 0U;
	if (string == 0) return 0U;
	while (string[length] != '\0') length++;
	return length;
}

static bool
boot_arg_token_equal(const char *token, uint32_t token_length, const char *argument)
{
	uint32_t argument_length = boot_arg_string_length(argument);
	return token_length == argument_length && memcmp(token, argument, token_length) == 0;
}

bool
boot_args_init(void)
{
	if (g_boot_args_initialized) return false;

	uint32_t length = 0U;
	nvram_status_t status = nvram_get(NVRAM_BOOT_ARGS_NAME, g_boot_args, sizeof(g_boot_args), &length);

	if (status == NVRAM_STATUS_NOT_FOUND) {
		g_boot_args[0] = '\0';
	} else if (status != NVRAM_STATUS_OK) {
		return false;
	}

	g_boot_args_initialized = true;
	return true;
}

const char *
boot_args_raw(void)
{
	return g_boot_args_initialized ? g_boot_args : "";
}

bool
boot_arg_present(const char *argument)
{
	if (!g_boot_args_initialized || argument == 0 || argument[0] == '\0') return false;

	const char *cursor = g_boot_args;
	while (*cursor != '\0') {
		while (boot_arg_separator(*cursor)) cursor++;
		if (*cursor == '\0') break;

		const char *token = cursor;
		while (*cursor != '\0' && !boot_arg_separator(*cursor)) cursor++;
		if (boot_arg_token_equal(token, (uint32_t)(cursor - token), argument)) return true;
	}

	return false;
}

bool
boot_arg_value(const char *name, char *value, uint32_t capacity)
{
	if (!g_boot_args_initialized || name == 0 || value == 0 || capacity == 0U) return false;

	uint32_t name_length = boot_arg_string_length(name);
	const char *cursor = g_boot_args;

	while (*cursor != '\0') {
		while (boot_arg_separator(*cursor)) cursor++;
		if (*cursor == '\0') break;

		const char *token = cursor;
		while (*cursor != '\0' && !boot_arg_separator(*cursor)) cursor++;
		uint32_t token_length = (uint32_t)(cursor - token);

		if (token_length > name_length && token[name_length] == '=' && memcmp(token, name, name_length) == 0) {
			uint32_t result_length = token_length - name_length - 1U;
			if (result_length + 1U > capacity) return false;
			memcpy(value, token + name_length + 1U, result_length);
			value[result_length] = '\0';
			return true;
		}
	}

	return false;
}

bool
boot_arg_bool(const char *name, bool default_value)
{
	char value[8];
	if (!boot_arg_value(name, value, sizeof(value))) return default_value;
	if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0) return true;
	if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0) return false;
	return default_value;
}

bool
boot_args_verbose(void)
{
	return boot_arg_present("-v");
}

bool
boot_args_safe_mode(void)
{
	return boot_arg_present("-x");
}

bool
boot_args_component_disabled(boot_component_t component)
{
	switch (component) {
	case BOOT_COMPONENT_GPU: return boot_arg_present("-no-gpu");
	case BOOT_COMPONENT_INPUT: return boot_arg_present("-no-input");
	case BOOT_COMPONENT_BLOCK: return boot_arg_present("-no-block");
	case BOOT_COMPONENT_LOGD: return boot_arg_present("-no-logd");
	case BOOT_COMPONENT_PATCHD: return boot_arg_present("-no-patchd");
	default: return false;
	}
}

void
boot_args_dump(void)
{
	kputs("boot-args: ");
	kputln(g_boot_args[0] != '\0' ? g_boot_args : "<none>");
	kprintf("boot-args: verbose=%u safe=%u gpu=%s input=%s block=%s logd=%s patchd=%s\n",
		boot_args_verbose() ? 1U : 0U,
		boot_args_safe_mode() ? 1U : 0U,
		boot_args_component_disabled(BOOT_COMPONENT_GPU) ? "off" : "on",
		boot_args_component_disabled(BOOT_COMPONENT_INPUT) ? "off" : "on",
		boot_args_component_disabled(BOOT_COMPONENT_BLOCK) ? "off" : "on",
		boot_args_component_disabled(BOOT_COMPONENT_LOGD) ? "off" : "on",
		boot_args_component_disabled(BOOT_COMPONENT_PATCHD) ? "off" : "on");
}
